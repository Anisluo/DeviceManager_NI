#pragma once

#include "core/PluginModule.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

struct PluginInfo;
class Station;
class ScriptEngine;
class DataStore;

// Owns the embedded CPython interpreter (pybind11). After start() the main
// thread does not hold the GIL; every entry into Python acquires it.
//
// Must outlive every Python object: create it before the AppController and
// destroy it after (main.cpp does this by declaration order).
class PythonRuntime {
public:
    using LogSink = std::function<void(const QString& level, const QString& source, const QString& message)>;

    PythonRuntime();
    ~PythonRuntime();

    static PythonRuntime* instance();

    // home: Python installation (contains Lib/); extraPaths are prepended to sys.path.
    bool start(const QString& home, const QStringList& extraPaths, QString* error);
    bool isRunning() const;
    QString version() const;

    void setStation(Station* station);
    Station* station() const;
    void setScriptEngine(ScriptEngine* engine);
    ScriptEngine* scriptEngine() const;
    void setDataStore(DataStore* store);
    DataStore* dataStore() const;
    void setLogSink(LogSink sink);
    void log(const QString& level, const QString& source, const QString& message) const;

    std::shared_ptr<PluginModule> loadPlugin(const PluginInfo& info, QString* error);
    // module.function(**json.loads(argsJson)) -> json.dumps(result); "" and *error on failure.
    QString callJson(const QString& module, const QString& function, const QString& argsJson, QString* error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
