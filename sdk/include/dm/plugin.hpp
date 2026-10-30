// dm/plugin.hpp — C++ convenience layer over dm_plugin.h.
//
// Derive from dm::Plugin, register methods in the constructor, then put
// DM_EXPORT_PLUGIN(YourClass, kManifestJson) in exactly one .cpp file.
// The macro generates the C exports, turns exceptions into {"error":...}
// results and implements the "$fetch" / "$methods" conventions.
//
//   class MySmu : public dm::Plugin {
//   public:
//       explicit MySmu(const dm::Json& cfg) {
//           method("set_voltage", "Set output voltage", {"channel","voltage"},
//                  [this](const dm::Json& a) { ...; return dm::Json{{"ok", true}}; });
//       }
//   };
//   DM_EXPORT_PLUGIN(MySmu, R"({"id":"my_smu", ...})")
#pragma once

#include "../dm_plugin.h"
#include "json.hpp"

#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace dm {

// Throw from a method handler to return a specific ABI error code.
class PluginError : public std::runtime_error {
public:
    PluginError(int code, const std::string& msg) : std::runtime_error(msg), code_(code) {}
    int code() const { return code_; }
private:
    int code_;
};

inline void require(bool cond, const std::string& msg) {
    if (!cond) throw PluginError(DM_E_BAD_ARGS, msg);
}

class Plugin {
public:
    using Handler = std::function<Json(const Json& args)>;

    virtual ~Plugin() = default;

    // Emit an async event to the host. Safe to call from any thread.
    void emit_event(const Json& evt) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        if (cb_) { std::string s = evt.dump(); cb_(s.c_str(), cb_user_); }
    }
    void log(const std::string& level, const std::string& msg) {
        emit_event(Json{{"type", "log"}, {"level", level}, {"message", msg}});
    }

    // Call a method on another device of the station (needs a host that supports
    // plugin_set_host). Throws PluginError with the device's error code and message.
    Json host_call(const std::string& device, const std::string& method, const Json& args = Json::object()) {
        if (!host_ || !host_->invoke)
            throw PluginError(DM_E_STATE, "host does not offer device calls (plugin_set_host)");
        std::string result;
        const int rc = host_->invoke(host_->ctx, device.c_str(), method.c_str(), args.dump().c_str(),
                                     [](const char* json, void* user) { *static_cast<std::string*>(user) = json ? json : ""; },
                                     &result);
        Json r = result.empty() ? Json::object() : Json::parse(result);
        if (rc < 0) {
            const std::string msg = r.is_object() && r.contains("error") ? r["error"].as_string() : std::string("call failed");
            throw PluginError(rc, device + "." + method + ": " + msg);
        }
        return r;
    }
    bool has_host() const { return host_ && host_->invoke; }

    // ---- used by the generated C exports ----
    void set_callback(dm_event_cb cb, void* user) {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        cb_ = cb; cb_user_ = user;
    }
    void set_host(const dm_host_api* api) { host_ = api; }

    int invoke(const char* method, const char* args_json, char* out, int out_len) {
        std::string name = method ? method : "";
        if (name == DM_METHOD_FETCH) return write_out(out, out_len, pending_, /*keep=*/false);

        std::string result;
        int rc = DM_OK;
        try {
            Json args = (args_json && *args_json) ? Json::parse(args_json) : Json::object();
            if (name == DM_METHOD_METHODS) {
                result = describe_methods().dump();
            } else {
                const Entry* e = find(name);
                if (!e) throw PluginError(DM_E_NO_METHOD, "unknown method: " + name);
                result = e->fn(args).dump();
            }
        } catch (const PluginError& ex) {
            rc = ex.code();
            result = Json{{"error", ex.what()}}.dump();
        } catch (const JsonError& ex) {
            rc = DM_E_BAD_ARGS;
            result = Json{{"error", ex.what()}}.dump();
        } catch (const std::exception& ex) {
            rc = DM_E_FAIL;
            result = Json{{"error", ex.what()}}.dump();
        } catch (...) {
            rc = DM_E_FAIL;
            result = R"({"error":"unknown exception"})";
        }
        if (rc != DM_OK) {
            write_out(out, out_len, result, false);   // best effort, error text may truncate
            return rc;
        }
        return write_out(out, out_len, result, /*keep=*/true);
    }

protected:
    // Register a method. `arg_names` is documentation for the host / page editor.
    void method(const std::string& name, const std::string& doc,
                std::vector<std::string> arg_names, Handler fn) {
        entries_.push_back({name, doc, std::move(arg_names), std::move(fn)});
    }

private:
    struct Entry {
        std::string name, doc;
        std::vector<std::string> args;
        Handler fn;
    };

    const Entry* find(const std::string& n) const {
        for (auto& e : entries_) if (e.name == n) return &e;
        return nullptr;
    }

    Json describe_methods() const {
        Json list = Json::array();
        for (auto& e : entries_) {
            Json args = Json::array();
            for (auto& a : e.args) args.push_back(a);
            list.push_back(Json{{"name", e.name}, {"doc", e.doc}, {"args", args}});
        }
        return list;
    }

    int write_out(char* out, int out_len, const std::string& s, bool keep) {
        const int len = int(s.size());          // `s` may alias pending_
        if (out && out_len > 0 && len < out_len) {
            std::memcpy(out, s.c_str(), size_t(len) + 1);
            pending_.clear();
            return len;
        }
        if (keep) {
            pending_ = s;
            if (out && out_len >= 32) std::snprintf(out, size_t(out_len), "{\"need\":%d}", int(s.size()) + 1);
            return DM_E_BUFFER;
        }
        if (out && out_len > 0) { std::memcpy(out, s.c_str(), size_t(out_len - 1)); out[out_len - 1] = 0; }
        return s.empty() ? DM_E_STATE : DM_E_BUFFER;
    }

    std::vector<Entry> entries_;
    std::string pending_;
    std::mutex cb_mutex_;
    dm_event_cb cb_ = nullptr;
    void* cb_user_ = nullptr;
    const dm_host_api* host_ = nullptr;
};

} // namespace dm

#define DM_EXPORT_PLUGIN(Class, ManifestJson)                                              \
    extern "C" {                                                                           \
    PLUGIN_API int plugin_abi_version(void) { return DM_ABI_VERSION; }                     \
    PLUGIN_API const char* plugin_manifest(void) { return ManifestJson; }                  \
    PLUGIN_API void* plugin_create(const char* config_json) {                              \
        try {                                                                              \
            ::dm::Json cfg = (config_json && *config_json) ? ::dm::Json::parse(config_json) \
                                                           : ::dm::Json::object();         \
            return static_cast<::dm::Plugin*>(new Class(cfg));                             \
        } catch (...) { return nullptr; }                                                  \
    }                                                                                      \
    PLUGIN_API int plugin_invoke(void* h, const char* m, const char* a, char* o, int n) {  \
        if (!h) return DM_E_STATE;                                                         \
        return static_cast<::dm::Plugin*>(h)->invoke(m, a, o, n);                          \
    }                                                                                      \
    PLUGIN_API void plugin_destroy(void* h) { delete static_cast<::dm::Plugin*>(h); }      \
    PLUGIN_API void plugin_set_callback(void* h, dm_event_cb cb, void* user) {             \
        if (h) static_cast<::dm::Plugin*>(h)->set_callback(cb, user);                      \
    }                                                                                      \
    PLUGIN_API void plugin_set_host(void* h, const dm_host_api* api) {                     \
        if (h) static_cast<::dm::Plugin*>(h)->set_host(api);                               \
    }                                                                                      \
    }
