#include "Device.h"

#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>
#include <QPointer>

#include <cstring>
#include <vector>

static QJSEngine* g_jsEngine = nullptr;

void Device::setJsEngine(QJSEngine* engine) { g_jsEngine = engine; }

Device::Device(QString name, QString pluginId, QJsonObject config, QObject* parent)
    : QObject(parent), m_name(std::move(name)), m_pluginId(std::move(pluginId)), m_config(std::move(config))
{
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(-1);
}

Device::~Device() { close(); }

void Device::mergeDefaults(const QJsonObject& defaults)
{
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        if (!m_config.contains(it.key())) {
            m_config.insert(it.key(), it.value());
        } else if (it.value().isObject() && m_config.value(it.key()).isObject()) {
            QJsonObject merged = it.value().toObject();
            const QJsonObject own = m_config.value(it.key()).toObject();
            for (auto o = own.begin(); o != own.end(); ++o) merged.insert(o.key(), o.value());
            m_config.insert(it.key(), merged);
        }
    }
}

bool Device::open(std::shared_ptr<PluginModule> lib)
{
    close();
    m_lib = std::move(lib);
    if (!m_lib) { setStatus("error", m_lastError.isEmpty() ? QStringLiteral("no library") : m_lastError); return false; }

    const QByteArray cfg = QJsonDocument(m_config).toJson(QJsonDocument::Compact);
    m_handle = m_lib->create(cfg.constData());
    if (!m_handle) {
        const QString why = m_lib->lastError();
        m_lib.reset();
        setStatus("error", QStringLiteral("创建插件实例失败：%1").arg(why.isEmpty() ? QString::fromUtf8(cfg) : why));
        return false;
    }
    m_lib->setCallback(m_handle, &Device::eventThunk, this);
    ++m_generation;
    setStatus("online");
    emit reopened();
    return true;
}

void Device::close()
{
    m_pool.waitForDone();
    QMutexLocker lk(&m_callMutex);
    if (m_handle && m_lib) {
        m_lib->setCallback(m_handle, nullptr, nullptr);
        m_lib->destroy(m_handle);
    }
    m_handle = nullptr;
    m_lib.reset();
    if (m_status == QLatin1String("online")) {
        m_status = QStringLiteral("offline");
        emit statusChanged();
    }
}

void Device::fail(const QString& error)
{
    close();
    setStatus("error", error);
}

void Device::setStatus(const QString& s, const QString& err)
{
    if (m_status == s && m_lastError == err) return;
    m_status = s;
    m_lastError = err;
    emit statusChanged();
    if (!err.isEmpty()) emit log("error", err);
}

Device::Result Device::invokeRaw(const QByteArray& method, const QByteArray& argsJson)
{
    QMutexLocker lk(&m_callMutex);
    if (!m_handle || !m_lib) return {DM_E_STATE, R"({"error":"device not open"})"};

    // Per device, guarded by m_callMutex. Not thread_local: MinGW runs thread_local destructors
    // from a TLS callback at thread exit, which corrupted the heap when the pool threads ended.
    std::vector<char>& buf = m_invokeBuf;
    if (buf.empty()) buf.resize(64 * 1024);
    int rc = m_lib->invoke(m_handle, method.constData(), argsJson.constData(), buf.data(), int(buf.size()));
    if (rc == DM_E_BUFFER) {
        const QJsonObject need = QJsonDocument::fromJson(QByteArray(buf.data())).object();
        const int n = need.value("need").toInt(int(buf.size()) * 4);
        buf.resize(size_t(qMax(n + 1, int(buf.size()) * 2)));
        rc = m_lib->invoke(m_handle, DM_METHOD_FETCH, "{}", buf.data(), int(buf.size()));
    }
    return {rc, QByteArray(buf.data(), int(strnlen(buf.data(), buf.size())))};
}

void Device::call(const QString& method, const QVariantMap& args, const QJSValue& cb)
{
    const quint64 id = m_nextCallId++;
    if (cb.isCallable()) m_callbacks.insert(id, cb);
    const QByteArray m = method.toUtf8();
    const QByteArray a = QJsonDocument(QJsonObject::fromVariantMap(args)).toJson(QJsonDocument::Compact);
    QPointer<Device> self(this);

    m_pool.start([this, self, id, m, a] {
        Result r = invokeRaw(m, a);
        QMetaObject::invokeMethod(this, [self, id, m, r] {
            if (!self) return;
            const QVariant value = QJsonDocument::fromJson(r.json).toVariant();
            const bool ok = r.rc >= 0;
            if (!ok) {
                // Log a failing call once, not on every poll.
                const QString err = QStringLiteral("%1 → %2 (%3)")
                                        .arg(QString::fromUtf8(m), value.toMap().value("error").toString()).arg(r.rc);
                if (err != self->m_lastCallError) emit self->log("error", err);
                self->m_lastCallError = err;
            } else if (!self->m_lastCallError.isEmpty() && self->m_lastCallError.startsWith(QString::fromUtf8(m) + " →")) {
                self->m_lastCallError.clear();
            }
            QJSValue cb = self->m_callbacks.take(id);
            if (cb.isCallable() && g_jsEngine) {
                // Convert via QJsonValue so nested arrays become real JS arrays (a QVariantList
                // would arrive as a sequence wrapper that fails Array.isArray()).
                const QJsonDocument doc = QJsonDocument::fromJson(r.json);
                const QJSValue js = doc.isArray() ? g_jsEngine->toScriptValue(doc.array())
                                  : doc.isObject() ? g_jsEngine->toScriptValue(doc.object())
                                                   : g_jsEngine->toScriptValue(value);
                cb.call({QJSValue(ok), js});
            }
        }, Qt::QueuedConnection);
    });
}

QVariant Device::invoke(const QString& method, const QVariantMap& args)
{
    const QByteArray a = QJsonDocument(QJsonObject::fromVariantMap(args)).toJson(QJsonDocument::Compact);
    Result r = invokeRaw(method.toUtf8(), a);
    return QJsonDocument::fromJson(r.json).toVariant();
}

QVariantList Device::methods()
{
    return QJsonDocument::fromJson(invokeRaw(DM_METHOD_METHODS, "{}").json).toVariant().toList();
}

void Device::eventThunk(const char* evt, void* user)
{
    auto* self = static_cast<Device*>(user);
    const QByteArray data(evt);
    // Plugin threads may call this; hop to the device's thread. Queued calls to a
    // deleted context object are dropped by Qt.
    QMetaObject::invokeMethod(self, [self, data] {
        const QVariantMap m = QJsonDocument::fromJson(data).toVariant().toMap();
        if (m.value("type").toString() == QLatin1String("log"))
            emit self->log(m.value("level", "info").toString(), m.value("message").toString());
        emit self->event(m);
    }, Qt::QueuedConnection);
}
