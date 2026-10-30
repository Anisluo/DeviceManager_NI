// foc_test <foc_motor plugin> — closed-loop checks against the simulated board through the C ABI.
#include <dm_plugin.h>
#include <dm/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
static void* open_lib(const char* p) { return (void*)LoadLibraryA(p); }
static void* sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
#else
#  include <dlfcn.h>
static void* open_lib(const char* p) { return dlopen(p, RTLD_NOW); }
static void* sym(void* h, const char* n) { return dlsym(h, n); }
#endif

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

static dm_invoke_fn invoke_fn;
static void* handle;

static dm::Json call(const char* method, const dm::Json& args = dm::Json::object(), int* rc_out = nullptr)
{
    std::vector<char> buf(1 << 20);
    std::string a = args.dump();
    int rc = invoke_fn(handle, method, a.c_str(), buf.data(), int(buf.size()));
    if (rc_out) *rc_out = rc;
    dm::Json r = dm::Json::parse(buf.data());
    if (rc < 0 && !rc_out) { std::printf("  %s -> %d %s\n", method, rc, buf.data()); ++failures; }
    return r;
}

static void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    void* lib = open_lib(argv[1]);
    if (!lib) { std::printf("cannot load %s\n", argv[1]); return 1; }
    auto create = (dm_create_fn)sym(lib, "plugin_create");
    invoke_fn = (dm_invoke_fn)sym(lib, "plugin_invoke");
    auto destroy = (dm_destroy_fn)sym(lib, "plugin_destroy");
    handle = create(R"({"simulate":true,"options":{"port":"sim"}})");
    CHECK(handle, "create");

    std::printf("discovery\n");
    dm::Json info = call("get_info");
    CHECK(info.boolean("connected", false) && info.integer("params", 0) >= 25, "params %d", info.integer("params", 0));
    CHECK(info["channels"].size() == 17, "channels %zu", info["channels"].size());
    dm::Json st = call("status");
    CHECK(!st.boolean("calibrated", true), "starts uncalibrated");

    std::printf("closed loop refused before calibration\n");
    int rc = 0;
    dm::Json r = call("run", dm::Json{{"mode", "SPEED"}, {"target", 1000}}, &rc);
    CHECK(rc == DM_E_STATE && r.str("error").find("未校准") != std::string::npos, "rc %d %s", rc, r.str("error").c_str());

    std::printf("encoder calibration\n");
    r = call("calibrate");
    CHECK(r.str("result") == "OK", "calibration %s", r.dump().c_str());

    std::printf("speed control 1000 rpm\n");
    call("run", dm::Json{{"mode", "SPEED"}, {"target", 1000}});
    wait_ms(800);
    st = call("status");
    CHECK(std::fabs(st.number("speed", 0) - 1000) < 20, "speed %.1f", st.number("speed", 0));
    CHECK(st.boolean("running", false), "running");

    std::printf("load step 0.25 N·m\n");
    call("sim_set_load", dm::Json{{"torque", 0.25}});
    wait_ms(600);
    st = call("status");
    CHECK(std::fabs(st.number("speed", 0) - 1000) < 25, "speed under load %.1f", st.number("speed", 0));
    CHECK(st.number("iq", 0) > 4.0 && st.number("iq", 0) < 6.0, "iq under load %.2f (expect ~4.8 A)", st.number("iq", 0));
    call("sim_set_load", dm::Json{{"torque", 0.0}});

    std::printf("speed step +500 rpm\n");
    r = call("step_test", dm::Json{{"loop", "speed"}, {"amplitude", 500}, {"base", 1000}});
    dm::Json m = r["metrics"];
    std::printf("  rise %.2f ms, overshoot %.1f %%, settle %s ms, error %.1f rpm\n", m.number("rise_ms", -1),
                m.number("overshoot_pct", -1), m["settle_ms"].dump().c_str(), m.number("error", 0));
    CHECK(m.number("rise_ms", 1e9) < 60, "speed rise");
    CHECK(m.number("overshoot_pct", 1e9) < 40, "speed overshoot");
    CHECK(std::fabs(m.number("error", 1e9)) < 25, "speed error");
    CHECK(r["channels"].size() == 3 && r["t"].size() == 600, "capture shape");

    std::printf("current step +3 A\n");
    r = call("step_test", dm::Json{{"loop", "current"}, {"amplitude", 3}, {"base", 0}});
    m = r["metrics"];
    std::printf("  rise %.3f ms, overshoot %.1f %%, error %.3f A\n", m.number("rise_ms", -1), m.number("overshoot_pct", -1),
                m.number("error", 0));
    CHECK(m.number("rise_ms", 1e9) < 1.0, "current rise < 1 ms");
    CHECK(std::fabs(m.number("error", 1e9)) < 0.2, "current error");

    std::printf("position step +90°\n");
    r = call("step_test", dm::Json{{"loop", "position"}, {"amplitude", 90}, {"base", 0}});
    m = r["metrics"];
    std::printf("  rise %.1f ms, overshoot %.1f %%, error %.2f°\n", m.number("rise_ms", -1), m.number("overshoot_pct", -1),
                m.number("error", 0));
    CHECK(std::fabs(m.number("error", 1e9)) < 2.0, "position error");
    call("command", dm::Json{{"name", "disable"}});

    std::printf("identification\n");
    wait_ms(300);
    r = call("identify");
    std::printf("  Rs %.4f Ω (model 0.35), Ld %.3e H (model 4.0e-4)\n", r.number("rs", 0), r.number("ld", 0));
    CHECK(std::fabs(r.number("rs", 0) / 0.35 - 1) < 0.1, "Rs within 10 %%");
    CHECK(std::fabs(r.number("ld", 0) / 0.40e-3 - 1) < 0.2, "Ld within 20 %%");

    std::printf("parameters\n");
    r = call("param_write", dm::Json{{"name", "cur_kp"}, {"value", 3.0}});
    CHECK(std::fabs(r.number("value", 0) - 3.0) < 1e-6, "cur_kp write");
    r = call("param_write", dm::Json{{"name", "cur_kp"}, {"value", 999.0}});
    CHECK(r.boolean("clamped", false) && std::fabs(r.number("value", 0) - 50) < 1e-6, "clamp to max");
    call("param_write", dm::Json{{"name", "rs_est"}, {"value", 1.0}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "read-only rejected");
    r = call("param_write", dm::Json{{"name", "enc_dir"}, {"value", "NORMAL"}});
    CHECK(r.integer("value", -1) == 0, "enum by name");
    r = call("params", dm::Json{{"group", "电流环"}});
    CHECK(r["rows"].size() == 3, "group filter %zu", r["rows"].size());
    call("command", dm::Json{{"name", "defaults"}});
    CHECK(std::fabs(call("param_read", dm::Json{{"name", "cur_kp"}}).number("value", 0) - 2.5) < 1e-6, "defaults restored");

    std::printf("free-running scope\n");
    call("scope_config", dm::Json{{"channels", "ia,ib,ic"}, {"divider", 2}, {"length", 500}, {"trigger", "none"}});
    r = call("scope_capture", dm::Json{{"force", true}});
    CHECK(r["channels"].size() == 3 && r["t"].size() == 500, "scope shape");

    std::printf("over-current fault\n");
    call("param_write", dm::Json{{"name", "oc_limit"}, {"value", 2.0}});
    call("run", dm::Json{{"mode", "CURRENT"}, {"target", 4.0}});
    wait_ms(100);
    st = call("status");
    CHECK(st.boolean("fault", false) && st.str("fault_text") == "过流", "fault %s", st.dump().c_str());
    call("param_write", dm::Json{{"name", "oc_limit"}, {"value", 20.0}});
    call("command", dm::Json{{"name", "clear_fault"}});
    CHECK(!call("status").boolean("fault", true), "fault cleared");

    destroy(handle);
    std::printf(failures ? "%d FAILURE(S)\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
