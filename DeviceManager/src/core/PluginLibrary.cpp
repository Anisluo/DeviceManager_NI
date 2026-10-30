#include "PluginLibrary.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

std::shared_ptr<PluginLibrary> PluginLibrary::load(const QString& path, const QString& shadowDir,
                                                   QString* error)
{
    std::shared_ptr<PluginLibrary> lib(new PluginLibrary);
    lib->m_path = path;

    QString loadPath = path;
    if (!shadowDir.isEmpty()) {
        QDir().mkpath(shadowDir);
        QFileInfo fi(path);
        const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
        lib->m_shadowPath = QDir(shadowDir).filePath(fi.completeBaseName() + "_" + stamp + "." + fi.suffix());
        if (QFile::copy(path, lib->m_shadowPath))
            loadPath = lib->m_shadowPath;
        else
            lib->m_shadowPath.clear();   // fall back to loading in place
    }

    lib->m_lib.setFileName(loadPath);
    if (!lib->m_lib.load()) {
        if (error) *error = lib->m_lib.errorString();
        return {};
    }

    auto abi      = reinterpret_cast<dm_abi_version_fn>(lib->m_lib.resolve("plugin_abi_version"));
    auto manifest = reinterpret_cast<dm_manifest_fn>(lib->m_lib.resolve("plugin_manifest"));
    lib->m_create      = reinterpret_cast<dm_create_fn>(lib->m_lib.resolve("plugin_create"));
    lib->m_invoke      = reinterpret_cast<dm_invoke_fn>(lib->m_lib.resolve("plugin_invoke"));
    lib->m_destroy     = reinterpret_cast<dm_destroy_fn>(lib->m_lib.resolve("plugin_destroy"));
    lib->m_setCallback = reinterpret_cast<dm_set_callback_fn>(lib->m_lib.resolve("plugin_set_callback"));
    lib->m_setHost     = reinterpret_cast<dm_set_host_fn>(lib->m_lib.resolve("plugin_set_host"));

    if (!manifest || !lib->m_create || !lib->m_invoke || !lib->m_destroy) {
        if (error) *error = QStringLiteral("missing required exports (plugin_manifest/create/invoke/destroy)");
        return {};
    }
    if (abi && abi() != DM_ABI_VERSION) {
        if (error) *error = QStringLiteral("ABI version %1, host expects %2").arg(abi()).arg(DM_ABI_VERSION);
        return {};
    }
    lib->m_manifest = QJsonDocument::fromJson(QByteArray(manifest())).object();
    return lib;
}

PluginLibrary::~PluginLibrary()
{
    m_lib.unload();
    if (!m_shadowPath.isEmpty())
        QFile::remove(m_shadowPath);
}

void* PluginLibrary::create(const char* configJson)
{
    void* h = m_create(configJson);
    m_lastError = h ? QString() : QStringLiteral("plugin_create returned NULL");
    return h;
}

int PluginLibrary::invoke(void* h, const char* method, const char* argsJson, char* out, int outLen)
{
    return m_invoke(h, method, argsJson, out, outLen);
}

void PluginLibrary::destroy(void* h) { m_destroy(h); }

void PluginLibrary::setCallback(void* h, dm_event_cb cb, void* user)
{
    if (m_setCallback) m_setCallback(h, cb, user);
}

void PluginLibrary::setHost(void* h, const dm_host_api* api)
{
    if (m_setHost) m_setHost(h, api);
}
