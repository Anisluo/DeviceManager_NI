#pragma once

#include <dm_plugin.h>

#include <QJsonObject>
#include <QString>

// A loaded plugin implementation. Mirrors the C ABI (dm_plugin.h) so devices
// treat C++ shared libraries and Python plugins the same way.
class PluginModule {
public:
    virtual ~PluginModule() = default;

    QString path() const { return m_path; }
    QJsonObject manifest() const { return m_manifest; }
    // Reason the last create() returned nullptr, if the backend knows it.
    QString lastError() const { return m_lastError; }

    virtual void* create(const char* configJson) = 0;
    virtual int   invoke(void* h, const char* method, const char* argsJson, char* out, int outLen) = 0;
    virtual void  destroy(void* h) = 0;
    virtual void  setCallback(void* h, dm_event_cb cb, void* user) = 0;

protected:
    QString m_path;
    QJsonObject m_manifest;
    QString m_lastError;
};
