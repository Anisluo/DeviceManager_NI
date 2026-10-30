#pragma once

#include "core/DataStore.h"
#include "core/LogModel.h"
#include "core/PluginRegistry.h"
#include "core/Station.h"
#include "python/ScriptEngine.h"

#include <QObject>
#include <QSettings>

// Root object exposed to QML as the `App` singleton.
class PythonRuntime;

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(Station* station READ station CONSTANT)
    Q_PROPERTY(PluginRegistry* plugins READ plugins CONSTANT)
    Q_PROPERTY(LogModel* log READ log CONSTANT)
    Q_PROPERTY(ScriptEngine* script READ script CONSTANT)
    Q_PROPERTY(DataStore* data READ data CONSTANT)
    Q_PROPERTY(bool darkTheme READ darkTheme WRITE setDarkTheme NOTIFY darkThemeChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
public:
    // python may be null (or not running): Python plugins and scripts are then unavailable.
    AppController(QStringList builtinRoots, QStringList binaryRoots, QString shadowDir,
                  QString stationsDir, QString sequencesDir, PythonRuntime* python,
                  QObject* parent = nullptr);
    ~AppController() override;

    Station* station() { return &m_station; }
    PluginRegistry* plugins() { return &m_registry; }
    LogModel* log() { return &m_log; }
    ScriptEngine* script() { return m_script; }
    DataStore* data() { return &m_data; }
    // Used when the station file has no "data_dir".
    void setDefaultDataDir(const QString& dir) { m_defaultDataDir = dir; }
    QString version() const;

    bool darkTheme() const { return m_dark; }
    void setDarkTheme(bool dark);

    Q_INVOKABLE bool loadStation(const QString& pathOrUrl);
    Q_INVOKABLE QStringList availableStations() const;
    Q_INVOKABLE void rescanPlugins();
    // Parsed page.json of a plugin (re-read from disk each call so edits show up).
    Q_INVOKABLE QVariantMap page(const QString& pluginId) const;
    Q_INVOKABLE QString toLocalPath(const QUrl& url) const;
    Q_INVOKABLE QUrl fileUrl(const QString& path) const;
    // Report (html | pdf) or Parquet export of a stored run, done in Python.
    Q_INVOKABLE bool exportRun(qint64 runId, const QString& format);

signals:
    void darkThemeChanged();
    void pageChanged(const QString& pluginId);
    void pluginReloaded(const QString& pluginId);

private:
    void applyRoots();

    QStringList m_builtinRoots, m_binaryRoots;
    QString m_stationsDir;
    PluginRegistry m_registry;
    Station m_station;
    LogModel m_log;
    DataStore m_data;
    QString m_defaultDataDir;
    PythonRuntime* m_python = nullptr;
    ScriptEngine* m_script = nullptr;
    QSettings m_settings;
    bool m_dark = false;
};
