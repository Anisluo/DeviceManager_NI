#pragma once

#include "PluginModule.h"

#include <QLibrary>

#include <memory>

// A C/C++ plugin shared library with its resolved C entry points.
//
// The library is loaded from a *shadow copy* so that the original file is not
// locked on Windows: rebuilding a plugin while the host runs is what makes hot
// reload possible.
class PluginLibrary : public PluginModule {
public:
    static std::shared_ptr<PluginLibrary> load(const QString& path, const QString& shadowDir,
                                               QString* error);
    ~PluginLibrary() override;

    void* create(const char* configJson) override;
    int   invoke(void* h, const char* method, const char* argsJson, char* out, int outLen) override;
    void  destroy(void* h) override;
    void  setCallback(void* h, dm_event_cb cb, void* user) override;
    void  setHost(void* h, const dm_host_api* api) override;

private:
    PluginLibrary() = default;
    QString m_shadowPath;
    QLibrary m_lib;
    dm_create_fn       m_create = nullptr;
    dm_invoke_fn       m_invoke = nullptr;
    dm_destroy_fn      m_destroy = nullptr;
    dm_set_host_fn     m_setHost = nullptr;
    dm_set_callback_fn m_setCallback = nullptr;
};
