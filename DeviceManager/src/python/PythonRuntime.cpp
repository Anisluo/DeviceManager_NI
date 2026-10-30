// pybind11 must come before Qt headers: Qt's `slots` macro clashes with Python's object.h.
#include <pybind11/embed.h>

#include "PythonRuntime.h"
#include "PythonPluginModule.h"
#include "ScriptEngine.h"
#include "core/DataStore.h"
#include "core/PluginRegistry.h"
#include "core/Station.h"

#include <QDir>
#include <QJsonDocument>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <chrono>
#include <thread>

namespace py = pybind11;

static PythonRuntime* g_runtime = nullptr;

struct PythonRuntime::Impl {
    std::unique_ptr<py::scoped_interpreter> interp;
    std::unique_ptr<py::gil_scoped_release> released;
    QPointer<Station> station;
    QPointer<ScriptEngine> engine;
    QPointer<DataStore> store;
    LogSink logSink;
    QString version;
    int pluginLoads = 0;
};

// ---------------------------------------------------------------------------
// _dmhost: the C++ side of the `dm` package. Everything that may block releases the GIL.
// ---------------------------------------------------------------------------
PYBIND11_EMBEDDED_MODULE(_dmhost, m)
{
    m.doc() = "DeviceManager host bindings (use the `dm` package instead)";

    m.def("devices", [] {
        py::list out;
        Station* st = g_runtime ? g_runtime->station() : nullptr;
        if (!st) return out;
        for (int i = 0; i < st->rowCount(); ++i) {
            const QModelIndex ix = st->index(i);
            py::dict d;
            d["name"] = st->data(ix, Station::NameRole).toString().toStdString();
            d["plugin"] = st->data(ix, Station::PluginRole).toString().toStdString();
            d["resource"] = st->data(ix, Station::ResourceRole).toString().toStdString();
            d["status"] = st->data(ix, Station::StatusRole).toString().toStdString();
            out.append(d);
        }
        return out;
    });

    m.def("invoke", [](const std::string& name, const std::string& method, const std::string& args) -> py::tuple {
        ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr;
        if (e && e->isScriptThread() && e->stopRequested()) {
            PyErr_SetNone(PyExc_KeyboardInterrupt);
            throw py::error_already_set();
        }
        Station* st = g_runtime ? g_runtime->station() : nullptr;
        Device* dev = st ? st->device(QString::fromStdString(name)) : nullptr;
        if (!dev)
            return py::make_tuple(DM_E_STATE, std::string(R"({"error":"no such device"})"));
        Device::Result r;
        {
            py::gil_scoped_release release;    // other Python threads (and Python plugins) may run
            r = dev->invokeRaw(QByteArray::fromStdString(method), QByteArray::fromStdString(args));
        }
        return py::make_tuple(int(r.rc), r.json.toStdString());
    });

    m.def("write", [](const std::string& stream, const std::string& text) {
        ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr;
        const QString s = QString::fromStdString(text);
        if (e && e->isScriptThread()) {
            e->postOutput(s, QString::fromStdString(stream));
        } else if (g_runtime && !s.trimmed().isEmpty()) {
            // print() from a Python plugin running on a device thread
            g_runtime->log(stream == "stderr" ? "error" : "info", "python", s.trimmed());
        }
    });

    m.def("log", [](const std::string& level, const std::string& message) {
        if (g_runtime) g_runtime->log(QString::fromStdString(level), "script", QString::fromStdString(message));
    });

    m.def("record", [](const std::string& json) {
        if (ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr)
            e->postRecord(QByteArray::fromStdString(json));
    });

    m.def("new_run", [](const std::string& name, const std::string& kind, const std::string& metaJson) -> qint64 {
        ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr;
        if (!e || !e->isScriptThread()) return 0;
        const QVariantMap meta = QJsonDocument::fromJson(QByteArray::fromStdString(metaJson)).toVariant().toMap();
        return e->newRun(QString::fromStdString(name), QString::fromStdString(kind), meta);
    });

    m.def("current_run", []() -> qint64 {
        ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr;
        return e && e->isScriptThread() ? e->scriptRunId() : 0;
    });

    m.def("db_path", [] {
        DataStore* s = g_runtime ? g_runtime->dataStore() : nullptr;
        return s ? s->path().toStdString() : std::string();
    });

    m.def("export_dir", [] {
        DataStore* s = g_runtime ? g_runtime->dataStore() : nullptr;
        return s ? s->exportDir().toStdString() : std::string();
    });

    m.def("file_exported", [](const std::string& path) {
        if (ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr)
            e->postFileExported(QString::fromStdString(path));
    });

    m.def("progress", [](double fraction, const std::string& text) {
        if (ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr)
            e->postProgress(fraction, QString::fromStdString(text));
    });

    m.def("stop_requested", [] {
        ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr;
        return e && e->isScriptThread() && e->stopRequested();
    });

    m.def("sleep", [](double seconds) {
        ScriptEngine* e = g_runtime ? g_runtime->scriptEngine() : nullptr;
        const bool interruptible = e && e->isScriptThread();
        bool stopped = false;
        {
            py::gil_scoped_release release;
            using clock = std::chrono::steady_clock;
            const auto end = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(seconds));
            while (clock::now() < end) {
                if (interruptible && e->stopRequested()) { stopped = true; break; }
                const clock::duration left = end - clock::now();
                std::this_thread::sleep_for(std::min<clock::duration>(left, std::chrono::milliseconds(20)));
            }
        }
        if (stopped) {
            PyErr_SetNone(PyExc_KeyboardInterrupt);
            throw py::error_already_set();
        }
    });
}

// ---------------------------------------------------------------------------

PythonRuntime::PythonRuntime() : d(std::make_unique<Impl>()) { g_runtime = this; }

PythonRuntime::~PythonRuntime()
{
    if (d->interp) {
        d->released.reset();          // re-acquire the GIL on this thread before finalising
        d->interp.reset();
    }
    g_runtime = nullptr;
}

PythonRuntime* PythonRuntime::instance() { return g_runtime; }

bool PythonRuntime::start(const QString& home, const QStringList& extraPaths, QString* error)
{
    if (!QFileInfo::exists(QDir(home).filePath("Lib/os.py")) && !QFileInfo::exists(QDir(home).filePath("lib")))  {
        if (error) *error = QStringLiteral("找不到 Python 标准库：%1（可用环境变量 DM_PYTHONHOME 指定）").arg(home);
        return false;
    }
    QString failure;
    try {
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        config.install_signal_handlers = 0;   // Ctrl+C belongs to the GUI, not to Python
        config.parse_argv = 0;
        const std::wstring whome = QDir::toNativeSeparators(home).toStdWString();
        PyConfig_SetString(&config, &config.home, whome.c_str());
        d->interp = std::make_unique<py::scoped_interpreter>(&config, 0, nullptr, false);

        py::module_ sys = py::module_::import("sys");
        py::list path = sys.attr("path");
        for (int i = int(extraPaths.size()) - 1; i >= 0; --i)
            path.insert(0, QDir::toNativeSeparators(extraPaths[i]).toStdString());
        d->version = QString::fromStdString(py::str(sys.attr("version")).cast<std::string>()).section(' ', 0, 0);

        py::module_::import("dm._host").attr("install_streams")();
    } catch (const std::exception& e) {
        // Only record the message here: a pybind11::error_already_set still owns Python objects
        // and must be destroyed (end of this handler) before the interpreter is finalised.
        failure = QString::fromUtf8(e.what());
    }
    if (!failure.isNull()) {
        if (error) *error = QStringLiteral("Python 初始化失败：%1").arg(failure);
        d->released.reset();
        d->interp.reset();
        return false;
    }
    d->released = std::make_unique<py::gil_scoped_release>();
    return true;
}

bool PythonRuntime::isRunning() const { return d->interp != nullptr; }
QString PythonRuntime::version() const { return d->version; }
void PythonRuntime::setStation(Station* station) { d->station = station; }
Station* PythonRuntime::station() const { return d->station; }
void PythonRuntime::setScriptEngine(ScriptEngine* engine) { d->engine = engine; }
ScriptEngine* PythonRuntime::scriptEngine() const { return d->engine; }
void PythonRuntime::setDataStore(DataStore* store) { d->store = store; }
DataStore* PythonRuntime::dataStore() const { return d->store; }
void PythonRuntime::setLogSink(LogSink sink) { d->logSink = std::move(sink); }

void PythonRuntime::log(const QString& level, const QString& source, const QString& message) const
{
    if (d->logSink) d->logSink(level, source, message);
}

QString PythonRuntime::callJson(const QString& module, const QString& function, const QString& argsJson, QString* error)
{
    if (!isRunning()) {
        if (error) *error = QStringLiteral("Python 运行时未启动");
        return {};
    }
    QString failure, out;
    {
        py::gil_scoped_acquire gil;
        try {
            py::module_ json = py::module_::import("json");
            py::object args = json.attr("loads")(argsJson.isEmpty() ? std::string("{}") : argsJson.toStdString());
            py::object fn = py::module_::import(module.toStdString().c_str()).attr(function.toStdString().c_str());
            py::object result = fn(**args);
            out = QString::fromStdString(json.attr("dumps")(result, py::arg("ensure_ascii") = false).cast<std::string>());
        } catch (const std::exception& e) {
            failure = QString::fromUtf8(e.what());     // drop Python objects before leaving the GIL scope
        }
    }
    if (!failure.isNull()) {
        if (error) *error = failure;
        return {};
    }
    return out;
}

std::shared_ptr<PluginModule> PythonRuntime::loadPlugin(const PluginInfo& info, QString* error)
{
    if (!isRunning()) {
        if (error) *error = QStringLiteral("Python 运行时未启动");
        return {};
    }
    // A fresh module name per load so hot reload re-executes the file.
    const QString moduleName = QStringLiteral("dmplugin_%1_%2").arg(info.id).arg(++d->pluginLoads);
    return PythonPluginModule::load(info, moduleName, error);
}
