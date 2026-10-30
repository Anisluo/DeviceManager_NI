// tdc_task_test <tdc_measure plugin> <plugin source dir>
// Drives the TDC测量 task through the C ABI with a fake host that plays the PXIe-6571 ("dig1").
#include <dm_plugin.h>
#include <dm/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

namespace fs = std::filesystem;
using dm::Json;
using Clock = std::chrono::steady_clock;

// One-member object. (kv("k", "v") with two string literals would pick std::string's
// iterator-range constructor.)
static Json kv(const char* key, Json value)
{
    Json j = Json::object();
    j[key] = std::move(value);
    return j;
}

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

// ---------------------------------------------------------------- fake host: a simulated 6571
struct Fake6571 {
    std::vector<std::string> calls;
    Json last_start;
    Clock::time_point t0;
    double seconds = 0;
    bool running = false, project = false;
} g_dig;

static int host_invoke(void*, const char* device, const char* method, const char* args, dm_sink_fn sink, void* user)
{
    auto reply = [&](int rc, const Json& j) { const std::string s = j.dump(); sink(s.c_str(), user); return rc; };
    if (std::string(device) != "dig1") return reply(DM_E_STATE, kv("error", std::string("no device ") + device));
    const std::string m = method;
    const Json a = Json::parse(args && *args ? args : "{}");
    g_dig.calls.push_back(m);
    if (m == "pattern_project") {
        g_dig.project = fs::exists(fs::u8path(a.str("pinmap"))) && a["timing"].size() >= 1;
        return reply(g_dig.project ? DM_OK : DM_E_BAD_ARGS, kv("ok", g_dig.project));
    }
    if (!g_dig.project) return reply(DM_E_STATE, kv("error", "no pattern project loaded"));
    if (m == "pattern_timing" || m == "pattern_specs") {
        const bool ok = fs::exists(fs::u8path(a.str("path")));
        return reply(ok ? DM_OK : DM_E_BAD_ARGS, ok ? kv("ok", true) : kv("error", "missing " + a.str("path")));
    }
    if (m == "pattern_start") {
        if (!fs::exists(fs::u8path(a.str("path")))) return reply(DM_E_BAD_ARGS, kv("error", "missing pattern"));
        g_dig.last_start = a;
        g_dig.t0 = Clock::now();
        g_dig.seconds = a.number("sim_seconds", 0);
        g_dig.running = true;
        return reply(DM_OK, kv("started", a.str("label")));
    }
    if (m == "pattern_status") {
        const bool done = !g_dig.running || std::chrono::duration<double>(Clock::now() - g_dig.t0).count() >= g_dig.seconds;
        if (done) g_dig.running = false;
        return reply(DM_OK, done ? Json{{"done", true}, {"pass", true}, {"fail_count", Json::object()}} : kv("done", false));
    }
    if (m == "pattern_abort") { g_dig.running = false; return reply(DM_OK, kv("aborted", true)); }
    return reply(DM_E_NO_METHOD, kv("error", "unknown method " + m));
}

static dm_host_api g_host{int(sizeof(dm_host_api)), nullptr, &host_invoke};

// ---------------------------------------------------------------- plugin access
static dm_invoke_fn invoke_fn;
static void* handle;

static Json call(const char* method, const Json& args = Json::object(), int* rc_out = nullptr)
{
    std::vector<char> buf(1 << 22);
    const std::string a = args.dump();
    if (std::getenv("TDC_DEBUG")) std::printf("  > %s %s\n", method, a.c_str());
    const int rc = invoke_fn(handle, method, a.c_str(), buf.data(), int(buf.size()));
    if (rc_out) *rc_out = rc;
    Json r = Json::parse(buf.data());
    if (rc < 0 && !rc_out) { std::printf("  %s -> %d %s\n", method, rc, buf.data()); ++failures; }
    return r;
}

static bool close_to(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static std::string read(const fs::path& p) { std::ifstream in(p, std::ios::binary); std::stringstream s; s << in.rdbuf(); return s.str(); }
static bool has_call(const char* m) { for (auto& c : g_dig.calls) if (c == m) return true; return false; }

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    void* lib = open_lib(argv[1]);
    if (!lib) { std::printf("cannot load %s\n", argv[1]); return 1; }
    auto create = (dm_create_fn)sym(lib, "plugin_create");
    invoke_fn = (dm_invoke_fn)sym(lib, "plugin_invoke");
    auto destroy = (dm_destroy_fn)sym(lib, "plugin_destroy");
    auto set_host = (dm_set_host_fn)sym(lib, "plugin_set_host");
    CHECK(set_host, "plugin_set_host exported");

    // work on a copy of the bundled project
    const fs::path src = fs::u8path(argv[2]);
    const fs::path tmp = fs::temp_directory_path() / ("tdc_task_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(tmp);
    fs::copy(src / "project" / "DemoPrj-TDC", tmp / "P", fs::copy_options::recursive);
    const Json cfg{{"plugin_dir", src.u8string()}, {"simulate", true}, {"instruments", kv("digital", "dig1")},
                   {"options", Json{{"project", (tmp / "P").u8string()}, {"work_dir", (tmp / "w").u8string()}, {"pattern", "TDC_M20_2"}}}};
    handle = create(cfg.dump().c_str());
    CHECK(handle, "create");
    if (!handle) return 1;
    set_host(handle, &g_host);

    std::printf("project\n");
    Json info = call("info");
    CHECK(info.integer("patterns", 0) == 24, "patterns %d", info.integer("patterns", 0));
    CHECK(info.str("current") == "TDC_M20_2", "current %s", info.str("current").c_str());
    CHECK(call("list_patterns")["patterns"].size() == 24, "list");
    CHECK(call("pins")["rows"].size() == 14, "pins");
    CHECK(call("timesets")["rows"].size() == 12, "timesets %zu", call("timesets")["rows"].size());

    std::printf("decode + timing of TDC_M20_2\n");
    Json s = call("summary");
    CHECK(s.str("start_pin") == "I1" && s.str("hit_pin") == "I2" && s.str("busy_pin") == "I0", "roles %s", s["roles"].dump().c_str());
    CHECK(s.integer("hit_count", 0) == 16, "hits %d", s.integer("hit_count", 0));
    CHECK(close_to(s.number("hit_period", 0), 2e-6, 1e-9), "period %g", s.number("hit_period", 0));
    CHECK(close_to(s.number("start_to_hit", 0), 200.01e-6, 0.02e-6), "s2h %g", s.number("start_to_hit", 0));
    CHECK(s.integer("loops", 0) == 60000, "loops %d", s.integer("loops", 0));
    CHECK(close_to(s.number("total", 0), s.number("iteration", 0) * 60000, s.number("iteration", 0) * 2), "total");
    Json v = call("vectors");
    CHECK(v["rows"].size() == 21, "vectors %zu", v["rows"].size());
    CHECK(std::as_const(v)["rows"][size_t(1)].str("instruction") == "set_loop(60000)", "v1 %s", std::as_const(v)["rows"][size_t(1)].str("instruction").c_str());
    CHECK(std::as_const(v)["rows"][size_t(4)].str("states") == "H001", "v4 %s", std::as_const(v)["rows"][size_t(4)].str("states").c_str());
    CHECK(std::as_const(v)["rows"][size_t(6)].str("states") == "X101", "v6 %s", std::as_const(v)["rows"][size_t(6)].str("states").c_str());
    CHECK(std::as_const(v)["rows"][size_t(11)].str("label") == "SendHit" && std::as_const(v)["rows"][size_t(11)].str("states") == "X011", "v11");
    CHECK(std::as_const(v)["rows"][size_t(0)].str("comment") == "I1->start, I2->Stop", "comment %s", std::as_const(v)["rows"][size_t(0)].str("comment").c_str());
    Json tl = call("timeline");
    CHECK(tl["channels"].size() == 4 && tl["t"].size() == std::as_const(tl)["channels"][size_t(0)]["data"].size() && tl["t"].size() > 10, "timeline");

    std::printf("other patterns\n");
    call("select", kv("name", "TDC"));
    s = call("summary");
    CHECK(s.integer("hit_count", 0) == 16 && s.integer("loops", 0) == 1, "TDC hits %d loops %d", s.integer("hit_count", 0), s.integer("loops", 0));
    call("select", kv("name", "TDC_002"));
    s = call("summary");
    CHECK(s.str("start_pin") == "I1" && s.str("hit_pin") == "I2" && s.integer("hit_count", 0) == 16, "TDC_002");
    call("select", kv("name", "TDC_Mode3"));
    CHECK(call("summary").str("notes_text").find("v15") != std::string::npos, "Mode3 label warning");
    call("select", kv("name", "TDC_1018"));
    CHECK(call("vectors")["rows"][size_t(3)].str("states") == "1", "single pin low nibble");
    const auto t0 = Clock::now();
    const Json res1 = call("list_patterns");
    for (auto& p : res1["patterns"].items()) { call("select", kv("name", p.str("value"))); call("summary"); }
    CHECK(std::chrono::duration<double>(Clock::now() - t0).count() < 3.0, "all patterns analyse quickly");

    std::printf("edit counts, save\n");
    call("select", kv("name", "TDC_M20_2"));
    Json counts = call("counts");
    const Json* hitloop = nullptr;
    for (auto& r : counts["rows"].items()) if (r.str("name").rfind("v10", 0) == 0) hitloop = &r;
    CHECK(hitloop && hitloop->integer("value", 0) == 16, "v10 count");
    Json sc = call("set_count", Json{{"name", "v10 set_loop"}, {"value", 4}});
    CHECK(sc.integer("value", 0) == 4 && !sc.boolean("clamped", true), "set_count");
    CHECK(call("summary").integer("hit_count", 0) == 4, "4 hits after edit");
    sc = call("set_count", Json{{"name", "v10 set_loop"}, {"value", 100000}});
    CHECK(sc.integer("value", 0) == 65535 && sc.boolean("clamped", false), "clamped");
    int rc = 0;
    call("set_count", Json{{"name", "v00"}, {"value", 3}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "no count on v00 (%d)", rc);
    call("set_count", Json{{"name", "v10 set_loop"}, {"value", 4}});
    Json saved = call("save_as", kv("name", "TDC_M20_2_4hit"));
    CHECK(fs::exists(fs::u8path(saved.str("saved"))), "saved file");
    {
        const std::string a = read(tmp / "P" / "Patterns" / "TDC_M20_2.digipat"), b = read(fs::u8path(saved.str("saved")));
        size_t diff = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) diff += a[i] != b[i];
        CHECK(a.size() == b.size() && diff == 1, "patch changes one byte (%zu)", diff);
    }
    s = call("summary");
    CHECK(s.str("pattern") == "TDC_M20_2_4hit" && s.integer("hit_count", 0) == 4 && !s.boolean("modified", true), "saved summary");
    CHECK(s.str("label") == "TDC_M20_2", "start label kept");

    std::printf("specs\n");
    call("set_spec", Json{{"name", "DC.VDD"}, {"value", 1.8}});
    bool vdd_ok = false;
    const Json res2 = call("specs");
    for (auto& r : res2["rows"].items())
        if (r.str("name") == "DC.VDD") vdd_ok = r.number("value", 0) == 1.8 && r.str("unit") == "V";
    CHECK(vdd_ok, "VDD 1.8 V");
    bool vih_ok = false;
    const Json res3 = call("levels");
    for (auto& r : res3["rows"].items())
        if (r.str("pin") == "I0") vih_ok = r.str("vih").rfind("1.8 V", 0) == 0;
    CHECK(vih_ok, "levels follow VDD");

    std::printf("burst through the host\n");
    call("select", kv("name", "TDC"));
    call("burst");
    CHECK(has_call("pattern_project") && has_call("pattern_timing") && has_call("pattern_start"), "host calls");
    CHECK(g_dig.last_start.str("label") == "TDC", "label %s", g_dig.last_start.str("label").c_str());
    wait_ms(50);
    Json st = call("status");
    CHECK(st.str("state") == "done" && st.str("result") == "PASS", "done %s %s", st.str("state").c_str(), st.str("result").c_str());
    call("select", kv("name", "TDC_M20_2"));
    st = call("burst");
    CHECK(st.str("state") == "running", "running");
    call("burst", Json::object(), &rc);
    CHECK(rc == DM_E_STATE, "second burst refused (%d)", rc);
    st = call("abort");
    CHECK(st.str("state") == "aborted" && st.str("result") == "ABORT" && has_call("pattern_abort"), "abort");
    call("burst", kv("single", true));
    CHECK(g_dig.last_start.str("path").find("_once.digipat") != std::string::npos, "single run uses a patched copy");
    wait_ms(60);
    CHECK(call("status").str("result") == "PASS", "single PASS");

    std::printf("manual trigger\n");
    {
        call("set_trigger_mode", kv("manual", true));
        call("select", kv("name", "TDC_M20_2"));
        call("set_count", Json{{"name", "v01 set_loop"}, {"value", 3}});      // 3 measurements
        Json st0 = call("burst");
        CHECK(st0.boolean("manual", false) && st0.boolean("waiting", false), "waits for TRIG");
        wait_ms(30);
        CHECK(call("status").integer("loops_done", -1) == 0, "no edge, no measurement");
        call("trigger");
        call("trigger");                                          // ~1.2 ms measurement: this edge comes too early
        wait_ms(30);
        Json m = call("status");
        CHECK(m.integer("loops_done", -1) == 1 && m.integer("missed", 0) == 1, "done %d missed %d",
              m.integer("loops_done", -1), m.integer("missed", 0));
        CHECK(m.str("state") == "running" && m.boolean("waiting", false), "back in the wait loop");
        call("trigger", kv("count", 5));                          // queued edges run back to back
        wait_ms(40);
        m = call("status");
        CHECK(m.str("state") == "done" && m.str("result") == "PASS" && m.integer("loops_done", 0) == 3, "manual done %s %d",
              m.str("state").c_str(), m.integer("loops_done", 0));
        call("trigger", Json::object(), &rc);
        CHECK(rc == DM_E_STATE, "trigger without a waiting run refused");
        call("revert");
        call("set_trigger_mode", kv("manual", false));
    }

    std::printf("generator\n");
    Json g = call("generate", Json{{"name", "GEN_A"}, {"clock_mhz", 100}, {"out1_khz", 1000}, {"out1_cycles", 3},
                                   {"out2_khz", 250}, {"out2_cycles", 8}, {"out2_delay_us", 5}, {"loops", 4}});
    CHECK(g.str("name") == "GEN_A" && g.integer("vectors", 0) > 10, "generate");
    s = call("summary");
    CHECK(s.str("start_pin") == "I1" && s.str("hit_pin") == "I2" && s.str("busy_pin") == "I0", "gen roles");
    CHECK(s.integer("start_count", 0) == 3 && s.integer("hit_count", 0) == 8 && s.integer("loops", 0) == 4, "gen counts %d %d %d",
          s.integer("start_count", 0), s.integer("hit_count", 0), s.integer("loops", 0));
    CHECK(close_to(s.number("start_period", 0), 1e-6, 1e-12) && close_to(s.number("hit_period", 0), 4e-6, 1e-12), "gen periods");
    CHECK(close_to(s.number("start_to_hit", 0), 5e-6, 1e-12), "gen delay %g", s.number("start_to_hit", 0));
    CHECK(s["gen"].integer("vectors", 0) == g.integer("vectors", -1), "gen info");
    v = call("vectors");
    CHECK(v.str("pins") == "I3 I0 I1 I2", "gen pins %s", v.str("pins").c_str());
    bool wait_ok = false, clock_ok = true;
    for (auto& r : v["rows"].items()) {
        if (r.str("label") == "WaitEdge") wait_ok = r.str("instruction").rfind("jump_if(0x28!, WaitEdge)", 0) == 0 && r.str("states") == "1H00";
        clock_ok = clock_ok && r.str("states")[0] == '1';
    }
    CHECK(wait_ok && clock_ok, "wait loop / clock");
    const fs::path sheet = tmp / "w" / "gen" / "tdcgen_10000ps_I3_I0.digitiming";
    CHECK(fs::exists(sheet) && read(sheet).find("<DriveReturnLow>") != std::string::npos, "RL timing sheet");
    call("generate", kv("name", "TDC_M20_2"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "name clash refused");
    call("generate", Json{{"name", "GEN_B"}, {"clk_pin", "I0"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "pin clash refused");
    g = call("generate", Json{{"name", "GEN_C"}, {"clock_mhz", 100}, {"out2_khz", 300}});
    CHECK(g.str("note").find("OUT2") != std::string::npos, "rounding reported");
    g = call("generate", Json{{"name", "GEN_N"}, {"clk_pin", ""}});
    CHECK(call("vectors").str("pins") == "I0 I1 I2", "no clock pin");
    call("generate", Json{{"name", "GEN_A"}, {"out2_cycles", 4}});
    call("burst");
    wait_ms(50);
    CHECK(call("status").str("result") == "PASS", "generated burst");
    Json sv = call("save_generated");
    CHECK(fs::exists(fs::u8path(sv.str("saved"))) && fs::exists(fs::u8path(sv.str("timing"))), "save_generated");
    bool listed = false;
    const Json res4 = call("list_patterns");
    for (auto& p : res4["patterns"].items()) listed = listed || p.str("value") == "GEN_A";
    CHECK(listed, "saved pattern listed");

    destroy(handle);

    std::printf("no instrument\n");
    const Json cfg2{{"plugin_dir", src.u8string()}, {"instruments", kv("digital", nullptr)},
                    {"options", Json{{"project", (tmp / "P").u8string()}, {"work_dir", (tmp / "w2").u8string()}}}};
    handle = create(cfg2.dump().c_str());
    set_host(handle, &g_host);
    Json err = call("burst", Json::object(), &rc);
    CHECK(rc == DM_E_STATE && err.str("error").find("PXIe-6571") != std::string::npos, "no 6571: %s", err.str("error").c_str());
    CHECK(call("summary").integer("hit_count", 0) > 0, "analysis works without an instrument");
    destroy(handle);

    std::error_code ec;
    fs::remove_all(tmp, ec);
    std::printf(failures ? "%d FAILURES\n" : "ALL OK\n", failures);
    return failures ? 1 : 0;
}
