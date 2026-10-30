#include "AppController.h"
#include "python/PythonRuntime.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

AppController::AppController(QStringList builtinRoots, QStringList binaryRoots, QString shadowDir,
                             QString stationsDir, QString sequencesDir, PythonRuntime* python,
                             QObject* parent)
    : QObject(parent),
      m_builtinRoots(std::move(builtinRoots)),
      m_binaryRoots(std::move(binaryRoots)),
      m_stationsDir(std::move(stationsDir)),
      m_station(&m_registry),
      m_python(python && python->isRunning() ? python : nullptr)
{
    m_script = new ScriptEngine(m_python != nullptr, m_python ? m_python->version() : QString(),
                                sequencesDir, &m_data, this);
    connect(&m_data, &DataStore::message, &m_log, [this](const QString& level, const QString& text) {
        m_log.append(level, "data", text);
    });
    connect(m_script, &ScriptEngine::log, this, [this](const QString& level, const QString& msg) {
        m_log.append(level, "script", msg);
    });
    if (m_python) {
        m_python->setStation(&m_station);
        m_python->setScriptEngine(m_script);
        m_python->setDataStore(&m_data);
        // Python may log from any thread; LogModel lives on the UI thread.
        m_python->setLogSink([this](const QString& level, const QString& source, const QString& msg) {
            QMetaObject::invokeMethod(&m_log, [this, level, source, msg] { m_log.append(level, source, msg); },
                                      Qt::QueuedConnection);
        });
        m_registry.setPythonLoader([this](const PluginInfo& info, QString* err) {
            return m_python->loadPlugin(info, err);
        });
        // Real chassis: dm.discover.ni() (NI System Configuration) lists the instruments.
        m_station.setDiscoverer([this](QString* note) {
            QString err;
            const QString text = m_python->callJson("dm.discover", "ni", "{}", &err);
            if (!err.isEmpty()) { *note = QStringLiteral("仪器检测失败：%1").arg(err); return QJsonArray(); }
            const QJsonObject r = QJsonDocument::fromJson(text.toUtf8()).object();
            *note = r.value("note").toString();
            return r.value("instruments").toArray();
        });
    }

    m_dark = m_settings.value("ui/dark", false).toBool();
    m_registry.setShadowDir(shadowDir);
    m_registry.setBinaryRoots(m_binaryRoots);

    connect(&m_registry, &PluginRegistry::message, this, [this](const QString& level, const QString& text) {
        m_log.append(level, "plugins", text);
    });
    connect(&m_station, &Station::deviceLog, &m_log, &LogModel::append);
    connect(&m_registry, &PluginRegistry::pageChanged, this, [this](const QString& id) {
        m_log.append("info", "plugins", QStringLiteral("页面已更新：%1/page.json").arg(id));
        emit pageChanged(id);
    });
    connect(&m_registry, &PluginRegistry::libraryChanged, this, [this](const QString& id) {
        m_station.reopenPlugin(id);
        emit pluginReloaded(id);
    });
}

AppController::~AppController()
{
    m_script->stop();
    delete m_script;          // joins the script thread while devices still exist
    m_script = nullptr;
    m_station.closeAll();
    if (m_python) {
        m_python->setScriptEngine(nullptr);
        m_python->setDataStore(nullptr);
        m_python->setStation(nullptr);
    }
}

QString AppController::version() const { return QCoreApplication::applicationVersion(); }

void AppController::setDarkTheme(bool dark)
{
    if (dark == m_dark) return;
    m_dark = dark;
    m_settings.setValue("ui/dark", dark);
    emit darkThemeChanged();
}

void AppController::applyRoots()
{
    QStringList roots = m_station.pluginPaths();
    for (const QString& r : m_builtinRoots)
        if (!roots.contains(r)) roots << r;
    m_registry.setSearchRoots(roots);
}

bool AppController::loadStation(const QString& pathOrUrl)
{
    const QString path = pathOrUrl.startsWith("file:") ? toLocalPath(QUrl(pathOrUrl)) : pathOrUrl;
    if (m_script && m_script->running()) {
        m_log.append("error", "station", QStringLiteral("脚本正在运行，停止后再切换工位"));
        return false;
    }
    QString err;
    m_station.closeAll();
    if (!m_station.load(path, &err)) {
        m_log.append("error", "station", err);
        return false;
    }
    applyRoots();
    m_registry.rescan();

    const QString dataDir = m_station.dataDir().isEmpty() ? m_defaultDataDir : m_station.dataDir();
    QString dbErr;
    m_data.setStationName(m_station.name());
    if (!m_data.open(QDir(dataDir).filePath("devicemanager.sqlite"), &dbErr))
        m_log.append("error", "data", QStringLiteral("无法打开数据库：%1").arg(dbErr));

    for (int i = 0; i < m_registry.rowCount(); ++i) {
        const QString e = m_registry.data(m_registry.index(i), PluginRegistry::ErrorRole).toString();
        if (!e.isEmpty())
            m_log.append("warning", "plugins",
                         m_registry.data(m_registry.index(i), PluginRegistry::IdRole).toString() + ": " + e);
    }
    m_station.openAll();
    m_log.append("info", "station",
                 QStringLiteral("工位已加载：%1（%2 台仪器，%3 个测试任务）")
                     .arg(m_station.name()).arg(m_station.instrumentCount())
                     .arg(m_station.count() - m_station.instrumentCount()));
    m_settings.setValue("station/last", m_station.path());
    return true;
}

QStringList AppController::availableStations() const
{
    QStringList out;
    QDir d(m_stationsDir);
    for (const QString& f : d.entryList({"*.json"}, QDir::Files, QDir::Name))
        out << d.absoluteFilePath(f);
    return out;
}

void AppController::rescanPlugins()
{
    applyRoots();
    m_registry.rescan();
    m_station.addNewTasks();
    m_log.append("info", "plugins", QStringLiteral("插件扫描完成：%1 个").arg(m_registry.count()));
}

QVariantMap AppController::page(const QString& pluginId) const
{
    const PluginInfo* p = m_registry.find(pluginId);
    if (!p) return {{"error", QStringLiteral("未知插件 %1").arg(pluginId)}};
    if (p->pagePath.isEmpty()) return {{"error", QStringLiteral("插件 %1 没有 page.json").arg(pluginId)}};
    QFile f(p->pagePath);
    if (!f.open(QIODevice::ReadOnly)) return {{"error", QStringLiteral("无法读取 %1").arg(p->pagePath)}};
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError)
        return {{"error", QStringLiteral("page.json 解析失败：%1 (offset %2)").arg(perr.errorString()).arg(perr.offset)}};
    return doc.object().toVariantMap();
}

QString AppController::toLocalPath(const QUrl& url) const { return url.toLocalFile(); }

QUrl AppController::fileUrl(const QString& path) const { return QUrl::fromLocalFile(path); }

bool AppController::exportRun(qint64 runId, const QString& format)
{
    if (!m_script->available()) {
        m_log.append("error", "data", QStringLiteral("导出报告需要 Python"));
        return false;
    }
    const QString fmt = format.toLower();
    const QString code = fmt == "parquet"
        ? QStringLiteral("import dm.data\ndm.data.export_parquet(%1)\n").arg(runId)
        : QStringLiteral("import dm.report\ndm.report.export(%1, fmt=%2)\n").arg(runId).arg("'" + fmt + "'");
    return m_script->runTask(code, QStringLiteral("导出 %1").arg(fmt.toUpper()));
}
