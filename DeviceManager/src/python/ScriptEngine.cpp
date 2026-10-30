#include <pybind11/embed.h>

#include "ScriptEngine.h"
#include "core/DataStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

namespace py = pybind11;

// Lives on the script thread. Holds the persistent console namespace.
class ScriptWorker : public QObject {
public:
    unsigned long pyThreadId = 0;
    py::object consoleNs;

    // Returns (ok, message).
    std::pair<bool, QString> runJob(bool script, const QString& code, const QString& name)
    {
        py::gil_scoped_acquire gil;
        pyThreadId = PyThread_get_thread_ident();
        PyThreadState_SetAsyncExc(pyThreadId, nullptr);   // discard a Stop aimed at the previous job
        try {
            py::module_ host = py::module_::import("dm._host");
            if (!consoleNs) consoleNs = host.attr("new_namespace")("<console>");
            py::tuple r;
            if (script) {
                py::object ns = host.attr("new_namespace")(name.toStdString());
                r = host.attr("run_script")(code.toStdString(), name.toStdString(), ns);
                // Make the script's variables available in the console afterwards.
                for (auto item : py::dict(ns))
                    if (py::str(item.first).cast<std::string>().rfind("__", 0) != 0)
                        consoleNs[item.first] = item.second;
            } else {
                r = host.attr("run_console")(code.toStdString(), consoleNs);
            }
            const bool ok = r[0].cast<bool>();
            const QString msg = QString::fromStdString(r[1].cast<std::string>());
            PyThreadState_SetAsyncExc(pyThreadId, nullptr);   // drop a Stop that arrived too late
            PyErr_Clear();
            return {ok, msg};
        } catch (py::error_already_set& e) {
            return {false, QString::fromUtf8(e.what())};
        }
    }

    void cleanup(DataStore* store)
    {
        if (store) store->closeThreadConnection();
        if (!Py_IsInitialized()) return;
        py::gil_scoped_acquire gil;
        consoleNs = py::object();
    }
};

ScriptEngine::ScriptEngine(bool available, QString pythonVersion, QString sequencesDir, DataStore* store,
                           QObject* parent)
    : QObject(parent), m_available(available), m_pythonVersion(std::move(pythonVersion)),
      m_sequencesDir(std::move(sequencesDir)), m_store(store)
{
    if (!m_available) return;
    m_thread = new QThread(this);
    m_thread->setObjectName("python-script");
    m_worker = new ScriptWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

ScriptEngine::~ScriptEngine()
{
    if (!m_thread) return;
    stop();
    QMetaObject::invokeMethod(m_worker, [w = m_worker, s = m_store] { w->cleanup(s); }, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
}

bool ScriptEngine::isScriptThread() const
{
    return m_thread && QThread::currentThread() == m_thread;
}

bool ScriptEngine::run(const QString& code, const QString& name, bool background)
{
    if (m_running) return start(Kind::Script, code, name);   // reports "already running"
    m_background = background;                                 // cleared when the job ends
    const bool ok = start(Kind::Script, code, name.isEmpty() ? QStringLiteral("<script>") : name);
    if (!ok) m_background = false;
    return ok;
}

bool ScriptEngine::runFile(const QString& path)
{
    const QString abs = resolvePath(path);
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) {
        emit output(QStringLiteral("无法打开 %1\n").arg(abs), "stderr");
        return false;
    }
    return start(Kind::Script, QString::fromUtf8(f.readAll()), abs);
}

bool ScriptEngine::exec(const QString& source)
{
    emit output(source, "input");
    return start(Kind::Console, source, "<console>");
}

bool ScriptEngine::start(Kind kind, const QString& code, const QString& name)
{
    if (!m_available) {
        emit output(QStringLiteral("Python 不可用（宿主未找到 Python 运行时）\n"), "stderr");
        return false;
    }
    if (m_running) {
        emit output(QStringLiteral("已有脚本在运行，请先停止\n"), "stderr");
        return false;
    }
    m_stop = false;
    m_running = true;
    ++m_job;
    m_currentName = kind == Kind::Script ? QFileInfo(name).fileName()
                  : kind == Kind::Task ? name : QStringLiteral("控制台");
    m_currentPath = kind == Kind::Script && QFileInfo(name).isAbsolute() ? name : QString();
    if (kind == Kind::Script) {
        clearRecords();
        m_uiRunId = 0;
        m_progress = 0;
        m_progressText.clear();
        emit progressChanged();
        emit output(QStringLiteral("▶ 运行 %1\n").arg(m_currentName), "info");
        emit log("info", QStringLiteral("开始运行脚本 %1").arg(m_currentName));
    }
    m_clock.start();
    m_jobKind = kind;
    m_jobName = kind == Kind::Script ? QFileInfo(name).completeBaseName() : QStringLiteral("console");
    m_jobPath = m_currentPath;
    emit runningChanged();

    QMetaObject::invokeMethod(m_worker, [this, kind, code, name] {
        auto [ok, message] = m_worker->runJob(kind != Kind::Console, code, name);
        finishRun(ok ? "ok" : message == QStringLiteral("已停止") ? "stopped" : "error");
        QMetaObject::invokeMethod(this, [this, kind, ok, message] { onJobDone(kind, ok, message); },
                                  Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    return true;
}

void ScriptEngine::onJobDone(Kind kind, bool ok, const QString& message)
{
    const double seconds = m_clock.elapsed() / 1000.0;
    m_running = false;
    m_background = false;
    m_stop = false;
    emit runningChanged();
    if (kind == Kind::Task) {
        if (!ok) emit output(QStringLiteral("✕ %1：%2\n").arg(m_currentName, message), "stderr");
    } else if (kind == Kind::Script) {
        const QString line = QStringLiteral("%1 %2 · %3 s · %4 行数据\n")
                                 .arg(ok ? "■" : "✕", message).arg(seconds, 0, 'f', 2).arg(m_records.size());
        emit output(line, ok ? "info" : "stderr");
        emit log(ok ? "info" : "warning", QStringLiteral("脚本 %1：%2").arg(m_currentName, message));
    } else if (!ok && message == QStringLiteral("已停止")) {
        emit output("KeyboardInterrupt\n", "stderr");
    }
    emit finished(ok, message, seconds);
}

bool ScriptEngine::runTask(const QString& code, const QString& label)
{
    if (m_running) return start(Kind::Task, code, label);   // reports "already running"
    m_background = true;                                   // exports don't pop the console open
    const bool ok = start(Kind::Task, code, label);
    if (!ok) m_background = false;
    return ok;
}

qint64 ScriptEngine::newRun(const QString& name, const QString& kind, const QVariantMap& meta)
{
    finishRun("ok");
    m_runId = m_store ? m_store->createRun(name, kind, m_jobPath, meta) : 0;
    const qint64 id = m_runId;
    QMetaObject::invokeMethod(this, [this, id] { m_uiRunId = id; clearRecords(); }, Qt::QueuedConnection);
    return id;
}

void ScriptEngine::finishRun(const QString& status)
{
    if (m_runId && m_store) m_store->finishRun(m_runId, status);
    m_runId = 0;
}

void ScriptEngine::postFileExported(const QString& path)
{
    QMetaObject::invokeMethod(this, [this, path] {
        emit output(QStringLiteral("已导出 %1\n").arg(QDir::toNativeSeparators(path)), "info");
        emit fileExported(path);
    }, Qt::QueuedConnection);
}

void ScriptEngine::stop()
{
    if (!m_running || !m_worker) return;
    m_stop = true;
    // dm.sleep() and device calls raise KeyboardInterrupt cooperatively. A script
    // stuck in a pure-Python loop gets an asynchronous KeyboardInterrupt instead.
    const quint64 job = m_job;
    QTimer::singleShot(300, this, [this, job] {
        if (!m_running || job != m_job || !Py_IsInitialized() || !m_worker->pyThreadId) return;
        py::gil_scoped_acquire gil;
        PyThreadState_SetAsyncExc(m_worker->pyThreadId, PyExc_KeyboardInterrupt);
    });
}

void ScriptEngine::postOutput(const QString& text, const QString& stream)
{
    QMetaObject::invokeMethod(this, [this, text, stream] { emit output(text, stream); }, Qt::QueuedConnection);
}

void ScriptEngine::postRecord(const QByteArray& json)
{
    // [[name, value], ...] — an array of pairs keeps the order the script used.
    QStringList order;
    QVariantMap row;
    for (const QJsonValue& pair : QJsonDocument::fromJson(json).array()) {
        const QJsonArray kv = pair.toArray();
        order << kv.at(0).toString();
        row.insert(kv.at(0).toString(), kv.at(1).toVariant());
    }
    // Persist on the calling (script) thread; the first record opens a run named after the script.
    if (m_store && isScriptThread() && m_jobKind != Kind::Task) {
        if (!m_runId) newRun(m_jobName, m_jobKind == Kind::Console ? "console" : "script", {});
        m_store->appendPoint(m_runId, order, row);
    }
    QMetaObject::invokeMethod(this, [this, row, order] {
        for (const QString& c : order)
            if (!m_columns.contains(c)) m_columns << c;
        m_records.push_back(row);
        emit recordsChanged();
        emit recordAdded(int(m_records.size()) - 1);
    }, Qt::QueuedConnection);
}

void ScriptEngine::postProgress(double fraction, const QString& text)
{
    QMetaObject::invokeMethod(this, [this, fraction, text] {
        m_progress = qBound(0.0, fraction, 1.0);
        m_progressText = text;
        emit progressChanged();
    }, Qt::QueuedConnection);
}

QVariantList ScriptEngine::record(int row) const
{
    QVariantList cells;
    if (row < 0 || row >= m_records.size()) return cells;
    const QVariantMap& r = m_records.at(row);
    for (const QString& c : m_columns) cells << r.value(c);
    return cells;
}

QVariantList ScriptEngine::column(const QString& name) const
{
    QVariantList out;
    out.reserve(m_records.size());
    for (const QVariantMap& r : m_records) out << r.value(name);
    return out;
}

void ScriptEngine::clearRecords()
{
    m_records.clear();
    m_columns.clear();
    emit recordsChanged();
}

QStringList ScriptEngine::sequences() const
{
    QStringList out;
    QDir d(m_sequencesDir);
    for (const QString& f : d.entryList({"*.py"}, QDir::Files, QDir::Name))
        out << d.absoluteFilePath(f);
    return out;
}

QString ScriptEngine::resolvePath(const QString& path) const
{
    if (path.isEmpty() || QFileInfo(path).isAbsolute()) return path;
    return QDir(m_sequencesDir).absoluteFilePath(path);
}

QString ScriptEngine::readText(const QString& path) const
{
    QFile f(resolvePath(path));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

bool ScriptEngine::writeText(const QString& path, const QString& text)
{
    QFile f(resolvePath(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit output(QStringLiteral("无法写入 %1\n").arg(f.fileName()), "stderr");
        return false;
    }
    f.write(text.toUtf8());
    emit output(QStringLiteral("已保存 %1\n").arg(QDir::toNativeSeparators(f.fileName())), "info");
    return true;
}
