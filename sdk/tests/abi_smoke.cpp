// abi_smoke <plugin-library>
// Loads a plugin through the plain C ABI and checks the contract the host relies on.
#include <dm_plugin.h>
#include <dm/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
static void* open_lib(const char* p) { return (void*)LoadLibraryA(p); }
static void* sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
static void close_lib(void* h) { FreeLibrary((HMODULE)h); }
#else
#  include <dlfcn.h>
static void* open_lib(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* sym(void* h, const char* n) { return dlsym(h, n); }
static void close_lib(void* h) { dlclose(h); }
#endif

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::atomic<int> events{0};
static void on_event(const char* evt, void*) { ++events; std::printf("  event: %s\n", evt); }

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: abi_smoke <plugin>\n"); return 2; }
    void* lib = open_lib(argv[1]);
    if (!lib) { std::printf("cannot load %s\n", argv[1]); return 1; }

    auto abi      = (dm_abi_version_fn)sym(lib, "plugin_abi_version");
    auto manifest = (dm_manifest_fn)sym(lib, "plugin_manifest");
    auto create   = (dm_create_fn)sym(lib, "plugin_create");
    auto invoke   = (dm_invoke_fn)sym(lib, "plugin_invoke");
    auto destroy  = (dm_destroy_fn)sym(lib, "plugin_destroy");
    auto setcb    = (dm_set_callback_fn)sym(lib, "plugin_set_callback");
    CHECK(abi && manifest && create && invoke && destroy && setcb);
    if (failures) return 1;

    CHECK(abi() == DM_ABI_VERSION);
    dm::Json m = dm::Json::parse(manifest());
    std::printf("manifest id=%s version=%s\n", m.str("id").c_str(), m.str("version").c_str());
    CHECK(!m.str("id").empty());

    void* h = create(R"({"resource":"SIM1","simulate":true})");
    CHECK(h != nullptr);
    setcb(h, on_event, nullptr);

    std::vector<char> buf(4096);
    auto call = [&](const char* method, const char* args) {
        int rc = invoke(h, method, args, buf.data(), int(buf.size()));
        std::printf("  %-18s %-40s -> %d %s\n", method, args, rc, buf.data());
        return rc;
    };

    CHECK(call("$methods", "{}") >= 0);
    CHECK(call("get_info", "{}") >= 0);
    CHECK(call("set_voltage", R"({"channel":0,"voltage":1.2})") >= 0);
    CHECK(call("set_output", R"({"channel":0,"enabled":true})") >= 0);
    CHECK(call("measure", R"({"channel":0})") >= 0);
    dm::Json r = dm::Json::parse(buf.data());
    CHECK(r.number("voltage", 0) > 1.19 && r.number("voltage", 0) < 1.21);
    CHECK(r.number("current", 0) > 1.1e-3 && r.number("current", 0) < 1.3e-3);   // 1.2 V / 1 kΩ

    // Compliance: 5 V into 1 kΩ with a 1 mA limit must clamp.
    call("set_current_limit", R"({"channel":0,"current_limit":0.001})");
    call("set_voltage", R"({"channel":0,"voltage":5})");
    call("measure", R"({"channel":0})");
    r = dm::Json::parse(buf.data());
    CHECK(r.boolean("compliance", false));
    CHECK(r.number("voltage", 9) < 1.05);

    // Errors.
    CHECK(call("no_such_method", "{}") == DM_E_NO_METHOD);
    CHECK(call("set_voltage", R"({"channel":0,"voltage":60})") == DM_E_BAD_ARGS);
    CHECK(call("set_voltage", "{not json") == DM_E_BAD_ARGS);

    // Small buffer -> DM_E_BUFFER, then $fetch returns the kept result.
    char small[40];
    int rc = invoke(h, "measure_all", "{}", small, int(sizeof small));
    CHECK(rc == DM_E_BUFFER);
    std::printf("  small buffer -> %d %s\n", rc, small);
    rc = invoke(h, "$fetch", "{}", buf.data(), int(buf.size()));
    CHECK(rc > 0 && dm::Json::parse(buf.data())["channels"].size() == 4);

    CHECK(events.load() > 0);
    setcb(h, nullptr, nullptr);
    destroy(h);
    close_lib(lib);

    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
