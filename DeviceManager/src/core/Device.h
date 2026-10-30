#pragma once

#include "PluginModule.h"

#include <QHash>
#include <QJSValue>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QThreadPool>

#include <vector>
#include <QVariant>

#include <memory>

class QJSEngine;

// A device session: one plugin instance bound to a resource name ("smu1" ->
// sim_smu @ "SIM1"). Calls are executed on a private single-thread pool, so
// they are serialised per device and never block the UI thread.
class Device : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString pluginId READ pluginId CONSTANT)
    Q_PROPERTY(QString resource READ resource CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)       // online | error | offline
    Q_PROPERTY(QString lastError READ lastError NOTIFY statusChanged)
    Q_PROPERTY(bool online READ online NOTIFY statusChanged)
    Q_PROPERTY(int generation READ generation NOTIFY reopened)        // bumps on every (re)open
public:
    Device(QString name, QString pluginId, QJsonObject config, QObject* parent = nullptr);
    ~Device() override;

    static void setJsEngine(QJSEngine* engine);

    QString name() const { return m_name; }
    QString pluginId() const { return m_pluginId; }
    QString resource() const { return m_config.value("resource").toString(); }
    QString status() const { return m_status; }
    QString lastError() const { return m_lastError; }
    bool online() const { return m_status == QLatin1String("online"); }
    int generation() const { return m_generation; }
    QJsonObject config() const { return m_config; }
    // Fills keys missing from the station config with the plugin's default_config.
    void mergeDefaults(const QJsonObject& defaults);

    // Creates the plugin instance. Any previous instance is closed first.
    bool open(std::shared_ptr<PluginModule> lib);
    void close();
    void fail(const QString& error);

    // Asynchronous call for QML: cb(ok, result) runs on the UI thread.
    Q_INVOKABLE void call(const QString& method, const QVariantMap& args = {},
                          const QJSValue& cb = QJSValue());
    // Blocking call (scripts, tools). Returns the result or {"error": ...}.
    Q_INVOKABLE QVariant invoke(const QString& method, const QVariantMap& args = {});
    Q_INVOKABLE QVariantList methods();

    struct Result { int rc = 0; QByteArray json; };
    Result invokeRaw(const QByteArray& method, const QByteArray& argsJson);

signals:
    void statusChanged();
    void reopened();
    void event(const QVariantMap& evt);
    void log(const QString& level, const QString& message);

private:
    static void eventThunk(const char* evt, void* user);
    void setStatus(const QString& s, const QString& err = {});

    QString m_name, m_pluginId;
    QJsonObject m_config;
    QString m_status = QStringLiteral("offline");
    QString m_lastError;
    int m_generation = 0;

    std::shared_ptr<PluginModule> m_lib;
    void* m_handle = nullptr;
    QMutex m_callMutex;          // serialises plugin_invoke across pool + sync callers
    QThreadPool m_pool;
    std::vector<char> m_invokeBuf;     // reply buffer for invokeRaw (under m_callMutex)
    quint64 m_nextCallId = 1;
    QHash<quint64, QJSValue> m_callbacks;
    QString m_lastCallError;     // suppresses repeats of the same failing call
};
