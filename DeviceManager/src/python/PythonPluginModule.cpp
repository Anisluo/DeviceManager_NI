#include <pybind11/embed.h>
#include <pybind11/functional.h>

#include "PythonPluginModule.h"
#include "core/PluginRegistry.h"

#include <cstring>
#include <string>

namespace py = pybind11;

struct PythonPluginModule::Impl {
    std::string moduleName;
    py::object cls;
};

namespace {

struct Handle {
    py::object inst;
    std::string pending;   // result kept after DM_E_BUFFER, returned by "$fetch"
};

py::module_ hostHelpers() { return py::module_::import("dm._host"); }

QString describe(py::error_already_set& e)
{
    try {
        return QString::fromStdString(hostHelpers().attr("format_exception")(e.value()).cast<std::string>());
    } catch (...) {
        return QString::fromUtf8(e.what());
    }
}

int writeOut(Handle* h, const std::string& s, char* out, int outLen, bool keep)
{
    const int len = int(s.size());
    if (out && outLen > 0 && len < outLen) {
        std::memcpy(out, s.c_str(), size_t(len) + 1);
        h->pending.clear();
        return len;
    }
    if (keep) {
        h->pending = s;
        if (out && outLen >= 32) std::snprintf(out, size_t(outLen), "{\"need\":%d}", len + 1);
        return DM_E_BUFFER;
    }
    if (out && outLen > 0) { std::memcpy(out, s.c_str(), size_t(outLen - 1)); out[outLen - 1] = 0; }
    return DM_E_BUFFER;
}

} // namespace

std::shared_ptr<PluginModule> PythonPluginModule::load(const PluginInfo& info, const QString& moduleName,
                                                       QString* error)
{
    std::shared_ptr<PythonPluginModule> m(new PythonPluginModule);
    m->d = std::make_unique<Impl>();
    m->d->moduleName = moduleName.toStdString();
    m->m_path = info.libraryPath;
    m->m_manifest = info.manifest;

    py::gil_scoped_acquire gil;
    try {
        m->d->cls = hostHelpers().attr("load_plugin_class")(
            info.libraryPath.toStdString(), m->d->moduleName, info.entry.section(':', 1).toStdString());
    } catch (py::error_already_set& e) {
        if (error) *error = describe(e);
        return {};
    }
    return m;
}

PythonPluginModule::~PythonPluginModule()
{
    if (!d || !Py_IsInitialized()) return;
    py::gil_scoped_acquire gil;
    d->cls = py::object();
    try {
        py::module_::import("sys").attr("modules").attr("pop")(d->moduleName, py::none());
    } catch (py::error_already_set&) {
    }
}

void* PythonPluginModule::create(const char* configJson)
{
    py::gil_scoped_acquire gil;
    try {
        auto* h = new Handle;
        h->inst = hostHelpers().attr("create_plugin")(d->cls, std::string(configJson ? configJson : ""));
        m_lastError.clear();
        return h;
    } catch (py::error_already_set& e) {
        m_lastError = describe(e);
        return nullptr;
    }
}

int PythonPluginModule::invoke(void* hv, const char* method, const char* argsJson, char* out, int outLen)
{
    auto* h = static_cast<Handle*>(hv);
    if (!h) return DM_E_STATE;
    if (method && std::strcmp(method, DM_METHOD_FETCH) == 0) {
        const std::string kept = h->pending;
        return writeOut(h, kept, out, outLen, false);
    }
    std::string result;
    int rc = DM_E_FAIL;
    {
        py::gil_scoped_acquire gil;
        try {
            py::tuple r = h->inst.attr("_dm_invoke_json")(std::string(method ? method : ""),
                                                          std::string(argsJson ? argsJson : ""));
            rc = r[0].cast<int>();
            result = r[1].cast<std::string>();
        } catch (py::error_already_set& e) {
            result = R"({"error":")" + describe(e).replace('"', '\'').replace('\n', ' ').toStdString() + "\"}";
        }
    }
    if (rc < 0) {
        writeOut(h, result, out, outLen, false);
        return rc;
    }
    return writeOut(h, result, out, outLen, true);
}

void PythonPluginModule::destroy(void* hv)
{
    auto* h = static_cast<Handle*>(hv);
    if (!h) return;
    py::gil_scoped_acquire gil;
    try {
        h->inst.attr("close")();
    } catch (py::error_already_set&) {
    }
    delete h;   // drops the instance while holding the GIL
}

void PythonPluginModule::setCallback(void* hv, dm_event_cb cb, void* user)
{
    auto* h = static_cast<Handle*>(hv);
    if (!h) return;
    py::gil_scoped_acquire gil;
    try {
        if (!cb) {
            h->inst.attr("_dm_set_emit")(py::none());
            return;
        }
        std::function<void(std::string)> emitFn = [cb, user](std::string evt) {
            py::gil_scoped_release release;
            cb(evt.c_str(), user);
        };
        h->inst.attr("_dm_set_emit")(py::cpp_function(emitFn));
    } catch (py::error_already_set&) {
    }
}
