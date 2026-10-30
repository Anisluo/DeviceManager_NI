#pragma once

#include "core/PluginModule.h"

#include <memory>

struct PluginInfo;

// A Python plugin (plugin.json "type": "python"). Presents the same contract as
// a C++ plugin library; every call acquires the GIL.
class PythonPluginModule : public PluginModule {
public:
    static std::shared_ptr<PluginModule> load(const PluginInfo& info, const QString& moduleName,
                                              QString* error);
    ~PythonPluginModule() override;

    void* create(const char* configJson) override;
    int   invoke(void* h, const char* method, const char* argsJson, char* out, int outLen) override;
    void  destroy(void* h) override;
    void  setCallback(void* h, dm_event_cb cb, void* user) override;

private:
    PythonPluginModule() = default;
    struct Impl;
    std::unique_ptr<Impl> d;
};
