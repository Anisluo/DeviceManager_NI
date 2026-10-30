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
    {
        // a period written as a formula over the specifications (timeset "signal", the first one)
        const fs::path sheet = tmp / "P" / "Timing" / "TDC.digitiming";
        std::string x = read(sheet);
        const std::string from = "<Period>1 \xC2\xB5s</Period>";
        const size_t at = x.find(from);
        if (at != std::string::npos) x.replace(at, from.size(), "<Period>Time.period*2</Period>");
        std::ofstream(sheet, std::ios::binary) << x;
    }
    const Json cfg{{"plugin_dir", src.u8string()}, {"simulate", true}, {"instruments", kv("digital", "dig1")},
                   {"options", Json{{"project", (tmp / "P").u8string()}, {"work_dir", (tmp / "w").u8string()}, {"pattern", "TDC_M20_2"},
                                     {"export_root", (tmp / "plugins").u8string()}}}};
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

    std::printf("timing sheet: drive formats, edges, checks\n");
    {
        const Json edges = call("timeset_edges");
        CHECK(edges["rows"].size() == 15, "pin edges %zu", edges["rows"].size());
        bool rl = false, formula = false;
        for (auto& r : edges["rows"].items()) {
            if (r.str("timeset") == "pulse_2us" && r.str("pin") == "I1")
                rl = r.str("format").rfind("RL", 0) == 0 && r.str("data") == "1 \xC2\xB5s" && r.str("return") == "2 \xC2\xB5s";
            if (r.str("timeset") == "signal") formula = r.str("period") == "4.444 \xC2\xB5s";   // Time.period = 2.222 µs
        }
        CHECK(rl, "pulse_2us I1 is RL 1..2 us");
        CHECK(formula, "period formula Time.period*2");
        call("set_spec", Json{{"name", "Time.period"}, {"value", 3}});
        formula = false;
        const Json edges2 = call("timeset_edges");
        for (auto& r : edges2["rows"].items())
            if (r.str("timeset") == "signal") formula = r.str("period") == "6 \xC2\xB5s";
        CHECK(formula, "period follows the spec");
        call("set_spec", Json{{"name", "Time.period"}, {"value", 2.222}});      // back to the file's value

        // RL: a repeated 1 is one pulse per cycle (TDC_1017: I1 in pulse_2us, 1 + 500 + 1 vectors)
        call("select", kv("name", "TDC_1017"));
        s = call("summary");
        CHECK(s.str("hit_pin") == "I1" && s.integer("hit_count", 0) >= 502 && s.integer("hit_count", 0) % 502 == 0,
              "RL pulses %s %d", s.str("hit_pin").c_str(), s.integer("hit_count", 0));
        CHECK(close_to(s.number("hit_width", 0), 1e-6, 1e-12), "RL width %g", s.number("hit_width", 0));
        const Json tl1017 = call("timeline");
        CHECK(std::as_const(tl1017)["channels"][size_t(1)].str("name").find("RL") != std::string::npos, "RL in trace name");
        const Json pe = call("timeset_edges", kv("pattern", true));
        CHECK(pe["rows"].size() == 3, "edges of the current pattern %zu", pe["rows"].size());

        call("select", kv("name", "TDC_2_T"));
        bool missing = false;
        const Json ck = call("checks");
        for (auto& r : ck["rows"].items()) missing = missing || r.str("item").find("working") != std::string::npos;
        CHECK(missing, "timeset working does not define I2: %s", ck.dump().c_str());
        call("select", kv("name", "TDC"));
        bool nolevel = false;
        const Json ck2 = call("checks");
        for (auto& r : ck2["rows"].items()) nolevel = nolevel || r.str("item").find("start") != std::string::npos;
        CHECK(nolevel, "pin start has no levels: %s", ck2.dump().c_str());
        call("select", kv("name", "TDC_M20_2"));
        CHECK(call("checks").integer("count", -1) == 0, "TDC_M20_2 clean: %s", call("checks").dump().c_str());
    }
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
        call("set_count", Json{{"name", "v16 repeat"}, {"value", 20}});     // longer measurement, so the 2nd edge is early
        const int meas_ms = int(call("summary").number("iteration", 0) * 1000) + 1;
        std::printf("  one measurement %d ms\n", meas_ms);
        Json st0 = call("burst");
        CHECK(st0.boolean("manual", false) && st0.boolean("waiting", false), "waits for TRIG");
        wait_ms(30);
        CHECK(call("status").integer("loops_done", -1) == 0, "no edge, no measurement");
        call("trigger");
        call("trigger");                                          // still measuring: this edge comes too early
        wait_ms(meas_ms + 30);
        Json m = call("status");
        CHECK(m.integer("loops_done", -1) == 1 && m.integer("missed", 0) == 1, "done %d missed %d",
              m.integer("loops_done", -1), m.integer("missed", 0));
        CHECK(m.str("state") == "running" && m.boolean("waiting", false), "back in the wait loop");
        call("trigger", kv("count", 5));                          // queued edges run back to back
        wait_ms(2 * meas_ms + 50);
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
    CHECK(fs::exists(sheet) && read(sheet).find("<ReturnToLow>") != std::string::npos &&
          read(sheet).find("DriveReturnLow") == std::string::npos, "RL timing sheet in the editor's format");
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

    std::printf("edit vectors\n");
    auto cell = [](int v, const char* field, const std::string& value) { return Json{{"v", v}, {"field", field}, {"value", value}}; };
    call("select", kv("name", "TDC_M20_2"));
    const Json v0 = call("vectors");
    const size_t n0 = v0["rows"].size();
    const double measure0 = call("summary").number("iteration", 0);
    const std::string file0 = read(tmp / "P" / "Patterns" / "TDC_M20_2.digipat");
    call("edit_vector", cell(0, "pin:I2", "1"));
    Json ev = call("vectors");
    CHECK(ev["modified"].as_bool() && ev["rows"].items()[0].str("states")[2] == '1', "state edited %s", ev["rows"].items()[0].str("states").c_str());
    call("edit_vector", cell(0, "pin:I2", "Q"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad state refused");
    call("vector_insert", kv("v", 4));                         // before the labels SendStart, SendHit, wait1s
    ev = call("vectors");
    CHECK(ev["rows"].size() == n0 + 1, "insert %zu", ev["rows"].size());
    CHECK(ev["rows"].items()[7].str("label") == "SendStart", "labels shift, %s", ev["rows"].items()[7].str("label").c_str());
    CHECK(ev["rows"].items()[5].str("instruction").empty(), "inserted vector has no instruction");
    CHECK(ev["rows"].items()[4].str("instruction") == v0["rows"].items()[4].str("instruction"), "jump_if target intact");
    call("edit_vector", cell(5, "instruction", "repeat(1000)"));
    call("edit_vector", cell(5, "comment", "加一段等待"));
    call("edit_vector", cell(5, "label", "Added"));
    call("edit_vector", cell(5, "timeset", "ten_us"));
    ev = call("vectors");
    CHECK(ev["rows"].items()[5].str("instruction") == "repeat(1000)" && ev["rows"].items()[5].str("timeset") == "ten_us", "instruction, timeset");
    CHECK(call("summary").number("iteration", 0) > measure0, "timing follows the edit");
    call("edit_vector", cell(5, "instruction", "repeat(0)"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "repeat(0) refused");
    call("edit_vector", cell(5, "instruction", "end_loop(NoSuchLabel)"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "unknown label refused");
    call("edit_vector", cell(5, "instruction", "whatever"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "unknown instruction refused");
    call("edit_vector", cell(5, "label", "SendHit"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "duplicate label refused");
    call("edit_vector", cell(7, "label", ""), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "referenced label kept");
    call("pattern_add_pin", kv("pin", "I4"));
    CHECK(call("vectors").str("pins") == "I0 I1 I2 I3 I4", "pin added");
    call("pattern_add_pin", kv("pin", "nope"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "pin outside the pin map refused");
    call("burst");
    CHECK(g_dig.last_start.str("path").find("run") != std::string::npos, "edited pattern runs from a working copy");
    call("abort");
    CHECK(read(tmp / "P" / "Patterns" / "TDC_M20_2.digipat") == file0, "nothing written before saving");
    call("save_as", kv("name", "EDITED"));
    const std::string edited_rows = call("vectors")["rows"].dump();
    CHECK(call("info").str("current") == "EDITED", "saved copy selected");
    call("select", kv("name", "TDC_M20_2"));
    CHECK(call("vectors")["rows"].size() == n0 && !call("vectors")["modified"].as_bool(), "original restored after save_as");
    call("select", kv("name", "EDITED"));
    ev = call("vectors");
    CHECK(ev["rows"].size() == n0 + 1 && ev.str("pins") == "I0 I1 I2 I3 I4", "saved file decodes");
    CHECK(ev["rows"].items()[5].str("label") == "Added" && ev["rows"].items()[5].str("comment") == "加一段等待" &&
          ev["rows"].items()[5].str("instruction") == "repeat(1000)" && ev["rows"].items()[5].str("timeset") == "ten_us", "saved vector");
    CHECK(ev["rows"].items()[0].str("states")[2] == '1' && ev["rows"].items()[0].str("states")[4] == 'X', "saved states");
    CHECK(ev["rows"].dump() == edited_rows, "saved file equals the edit");
    call("vector_delete", kv("v", 5));
    ev = call("vectors");
    CHECK(ev["rows"].items()[5].str("label") == "Added" && ev["rows"].size() == n0, "label moves to the next vector");
    call("pattern_remove_pin", kv("pin", "I4"));
    call("pattern_save");
    CHECK(!call("vectors")["modified"].as_bool() && call("vectors").str("pins") == "I0 I1 I2 I3", "saved in place");
    call("edit_vector", cell(1, "comment", "draft"));
    call("revert");
    CHECK(call("vectors")["rows"].items()[1].str("comment") != "draft", "revert drops the draft");

    std::printf("edit timing\n");
    const fs::path timing = tmp / "P" / "Timing" / "TDC.digitiming";
    call("set_period", Json{{"name", "ten_us"}, {"field", "period"}, {"value", "20 \xC2\xB5s"}});
    CHECK(read(timing).find("<Period>20 \xC2\xB5s</Period>") != std::string::npos, "period written");
    Json period_row;
    const Json ts_rows = call("timesets");
    for (auto& r : ts_rows["rows"].items()) if (r.str("name") == "ten_us") period_row = r;
    CHECK(period_row.str("period") == "20 \xC2\xB5s", "period reloaded");
    call("set_period", Json{{"name", "ten_us"}, {"value", "no such"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad period refused");
    call("add_timeset", Json{{"name", "fast"}, {"copy_of", "ten_ns"}});
    CHECK(call("timesets")["rows"].size() == ts_rows["rows"].size() + 1, "timeset added %zu", call("timesets")["rows"].size());
    call("add_edge", Json{{"timeset", "fast"}, {"pin", "I1"}});
    call("set_edge", Json{{"timeset", "fast"}, {"pin", "I1"}, {"field", "format"}, {"value", "RL"}});
    call("set_edge", Json{{"timeset", "fast"}, {"pin", "I1"}, {"field", "Return"}, {"value", "8 ns"}});
    call("set_edge", Json{{"timeset", "fast"}, {"pin", "I1"}, {"field", "Strobe"}, {"value", "Time.period/2"}});
    Json edge_row;
    const Json edge_rows = call("timeset_edges");
    for (auto& r : edge_rows["rows"].items()) if (r.str("timeset") == "fast" && r.str("pin") == "I1") edge_row = r;
    CHECK(edge_row.str("fmt") == "RL" && edge_row.str("Return") == "8 ns" && edge_row.str("Strobe") == "Time.period/2", "edge %s", edge_row.dump().c_str());
    call("set_edge", Json{{"timeset", "fast"}, {"pin", "I1"}, {"field", "Data"}, {"value", "not a time"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad edge refused");
    call("set_edge", Json{{"timeset", "fast"}, {"pin", "AllPins"}, {"field", "Return"}, {"value", "1 ns"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "NR has no Return");
    call("remove_edge", Json{{"timeset", "fast"}, {"pin", "I1"}});
    call("remove_timeset", kv("name", "fast"));
    call("remove_timeset", kv("name", "ten_us"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "timeset in use kept");
    const size_t calls_before = g_dig.calls.size();
    call("burst");
    bool reloaded = false;
    for (size_t k = calls_before; k < g_dig.calls.size(); ++k) reloaded = reloaded || g_dig.calls[k] == "pattern_project";
    CHECK(reloaded, "edited sheets reach the instrument on the next burst");
    call("set_period", Json{{"name", "ten_us"}, {"value", "10 \xC2\xB5s"}}, &rc);
    CHECK(rc == DM_E_STATE, "no edits during a burst");
    call("abort");

    std::printf("edit levels\n");
    const fs::path level_sheet = tmp / "P" / "Levels" / "ReadData.digilevels";
    call("set_level", Json{{"pin", "I1"}, {"field", "Vih"}, {"value", "0.9*DC.VDD"}});
    CHECK(read(level_sheet).find("<Vih>0.9*DC.VDD</Vih>") != std::string::npos, "level written");
    call("set_level", Json{{"pin", "I1"}, {"field", "TerminationMode"}, {"value", "Vterm"}});
    Json level_row;
    const Json level_rows = call("levels");
    for (auto& r : level_rows["rows"].items()) if (r.str("pin") == "I1") level_row = r;
    CHECK(level_row.str("Vih") == "0.9*DC.VDD" && level_row.str("mode") == "Vterm", "level %s", level_row.dump().c_str());
    call("set_level", Json{{"pin", "I1"}, {"field", "Vih"}, {"value", "abc"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad level refused");
    call("set_level", Json{{"pin", "I1"}, {"field", "TerminationMode"}, {"value", "Open"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad termination refused");
    call("add_level", kv("pin", "start"));
    CHECK(call("levels")["rows"].size() == 5, "level added");
    call("remove_level", kv("pin", "start"));

    std::printf("edit pin map\n");
    const fs::path pinmap = tmp / "P" / "TDC.pinmap";
    call("add_pin", kv("name", "CLKIN"));
    CHECK(call("pins")["rows"].size() == 15, "pin added");
    call("set_pin", Json{{"pin", "CLKIN"}, {"field", "channel"}, {"value", "20"}});
    call("set_pin", Json{{"pin", "CLKIN"}, {"field", "groups"}, {"value", "DataPins，Extra"}});
    Json pin_row;
    const Json pin_rows = call("pins");
    for (auto& r : pin_rows["rows"].items()) if (r.str("pin") == "CLKIN") pin_row = r;
    CHECK(pin_row.integer("channel", -1) == 20 && pin_row.str("groups") == "DataPins, Extra", "pin %s", pin_row.dump().c_str());
    call("set_pin", Json{{"pin", "CLKIN"}, {"field", "channel"}, {"value", "0"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "channel in use refused");
    call("set_pin", Json{{"pin", "CLKIN"}, {"field", "channel"}, {"value", "32"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "channel out of range refused");
    call("set_pin", Json{{"pin", "CLKIN"}, {"field", "groups"}, {"value", ""}});
    call("remove_pin", kv("pin", "CLKIN"));
    CHECK(read(pinmap).find("CLKIN") == std::string::npos, "pin gone from the sheet");
    call("remove_pin", kv("pin", "I0"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "pin in use kept");
    call("rename_instrument", Json{{"instrument", "PXI1Slot15"}, {"value", "PXI1Slot3"}});
    CHECK(read(pinmap).find("PXI1Slot15") == std::string::npos && read(pinmap).find("instrument=\"PXI1Slot3\" channel=\"0\"") != std::string::npos, "instrument renamed");

    std::printf("edit specifications\n");
    const fs::path spec_sheet = tmp / "P" / "Specifications" / "Specifications.specs";
    call("set_spec_text", Json{{"key", "DC.VDD"}, {"field", "definition"}, {"value", "2.5 V"}});
    CHECK(read(spec_sheet).find("<f:Definition>2.5 V</f:Definition>") != std::string::npos, "definition written");
    call("set_spec_text", Json{{"key", "Time.period"}, {"field", "comment"}, {"value", "基准周期"}});
    CHECK(read(spec_sheet).find("<f:Comment>基准周期</f:Comment>") != std::string::npos, "comment added");
    const Json spec_table = call("spec_rows");
    Json spec_row;
    for (auto& r : spec_table["rows"].items()) if (r.str("key") == "DC.VDD") spec_row = r;
    CHECK(spec_row.str("definition") == "2.5 V" && spec_row.str("value") == "2.5", "spec row %s", spec_row.dump().c_str());
    call("set_spec_text", Json{{"key", "DC.VDD"}, {"field", "definition"}, {"value", "nonsense"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad definition refused");
    call("add_spec", Json{{"key", "Extra.limit"}, {"definition", "1 mA"}});
    CHECK(read(spec_sheet).find("<Section name=\"Extra\">") != std::string::npos, "new section");
    call("add_spec", Json{{"key", "Extra.limit"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "duplicate spec refused");
    call("remove_spec", kv("key", "Extra.limit"));
    CHECK(read(spec_sheet).find("Extra.limit") == std::string::npos && read(spec_sheet).find("symbol=\"limit\"") == std::string::npos, "spec removed");
    const Json one_ts = call("timeset_edges", kv("timeset", "ten_us"));
    bool only_one = one_ts["rows"].size() > 0;
    for (auto& r : one_ts["rows"].items()) only_one = only_one && r.str("timeset") == "ten_us";
    CHECK(only_one, "edges of one timeset");

    std::printf("restore\n");
    call("restore_sheets");
    CHECK(read(pinmap).find("PXI1Slot15") != std::string::npos && read(pinmap).find("PXI1Slot3") == std::string::npos, "pin map restored");
    CHECK(read(level_sheet).find("0.9*DC.VDD") == std::string::npos, "levels restored");
    CHECK(read(spec_sheet).find("2.5 V") == std::string::npos, "specs restored");
    CHECK(read(timing).find("<Period>10 \xC2\xB5s</Period>") != std::string::npos, "timing restored");

    std::printf("spreadsheet edits\n");
    call("select", kv("name", "TDC_M20_2"));
    Json cells = Json::array();
    for (int r = 5; r <= 7; ++r) cells.push_back(cell(r, "pin:I3", "H"));
    call("edit_vectors", kv("cells", cells));
    ev = call("vectors");
    CHECK(ev["rows"].items()[5].str("states")[3] == 'H' && ev["rows"].items()[7].str("states")[3] == 'H', "range filled");
    call("vector_undo");
    ev = call("vectors");
    CHECK(ev["rows"].items()[5].str("states")[3] != 'H' && !ev["modified"].as_bool(), "one undo for the whole range");
    cells = Json::array();
    cells.push_back(cell(5, "pin:I3", "L"));
    cells.push_back(cell(6, "pin:I3", "Q"));                   // refused
    call("edit_vectors", kv("cells", cells), &rc);
    CHECK(rc == DM_E_BAD_ARGS && call("vectors")["rows"].items()[5].str("states")[3] != 'L', "all or none");
    const size_t rows_before = call("vectors")["rows"].size();
    call("vector_insert", Json{{"v", 6}, {"count", 3}});
    CHECK(call("vectors")["rows"].size() == rows_before + 3, "insert rows");
    call("vector_delete", Json{{"v", 7}, {"count", 2}});
    CHECK(call("vectors")["rows"].size() == rows_before + 1, "delete rows");
    call("vector_undo");
    call("vector_undo");
    CHECK(call("vectors")["rows"].size() == rows_before && !call("vectors")["modified"].as_bool(), "undo rows");
    call("pattern_add_pin", kv("pin", "I4"));
    call("vector_undo");
    CHECK(call("vectors").str("pins") == "I0 I1 I2 I3", "undo pin column");
    call("vector_undo", Json::object(), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "nothing left to undo");
    call("revert");

    std::printf("new project\n");
    call("project_new", kv("name", "Fresh"));
    CHECK(call("info").str("current") == "main", "new project opens its pattern");
    CHECK(call("project_list")["projects"].size() == 1, "project listed");
    call("project_new", kv("name", "Fresh"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "existing project refused");
    call("project_new", kv("name", "1 bad"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad project name refused");
    ev = call("vectors");
    CHECK(ev.str("pins") == "I0 I1 I2 I3 I4 I5 I6 I7" && ev["rows"].size() == 1, "new pattern %s", ev.str("pins").c_str());
    CHECK(ev["rows"].items()[0].str("instruction") == "halt" && ev["rows"].items()[0].str("timeset") == "default", "one halt vector");
    Json fresh_checks = call("checks");
    CHECK(fresh_checks.integer("count", -1) == 0, "new project is clean: %s", fresh_checks.dump().c_str());
    call("pattern_new", kv("name", "second"));
    CHECK(call("info").str("current") == "second" && call("list_patterns")["patterns"].size() >= 2, "pattern added");
    call("pattern_new", kv("name", "second"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "duplicate pattern refused");
    call("edit_vector", cell(0, "pin:I0", "1"));

    std::printf("export plugin\n");
    call("export_plugin", Json{{"id", "my_task"}, {"name", "我的任务"}}, &rc);
    CHECK(rc == DM_E_BAD_ARGS, "unsaved edits block the export");
    call("pattern_save");
    Json ex = call("export_plugin", Json{{"id", "my_task"}, {"name", "我的任务"}});
    const fs::path out = tmp / "plugins" / "my_task";
    CHECK(fs::u8path(ex.str("folder")) == out, "export folder %s", ex.str("folder").c_str());
    CHECK(fs::exists(out / "my_task.dll") && fs::exists(out / "project" / "Patterns" / "second.digipat"), "exported files");
    const Json ex_manifest = Json::parse(read(out / "plugin.json"));
    CHECK(ex_manifest.str("entry") == "my_task" && ex_manifest.str("category") == "task" && ex_manifest["requires"]["digital"]["models"].size() == 1, "manifest");
    bool generator_tab = false;
    const Json ex_page = Json::parse(read(out / "page.json"));
    for (auto& tab : ex_page["tabs"].items()) generator_tab = generator_tab || tab.str("title") == "信号生成";
    CHECK(!generator_tab && ex_page.str("title") == "我的任务", "exported page");
    call("export_plugin", kv("id", "my_task"));                // an earlier export is replaced
    fs::create_directories(tmp / "plugins" / "someone_elses");
    call("export_plugin", kv("id", "someone_elses"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "other folders are left alone");
    call("export_plugin", kv("id", "1 bad"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "bad id refused");
    call("project_open", kv("path", "NoSuchProject"), &rc);
    CHECK(rc == DM_E_BAD_ARGS, "missing project refused");
    call("project_open", kv("path", (tmp / "P").u8string()));
    CHECK(call("info").integer("patterns", 0) >= 24, "back in the first project");

    std::printf("exported plugin runs\n");
    const fs::path ex_dll = out / "my_task.dll";
    void* ex_lib = open_lib(ex_dll.string().c_str());
    CHECK(ex_lib, "exported DLL loads");
    if (ex_lib) {
        auto ex_create = (dm_create_fn)sym(ex_lib, "plugin_create");
        auto ex_set_host = (dm_set_host_fn)sym(ex_lib, "plugin_set_host");
        auto ex_destroy = (dm_destroy_fn)sym(ex_lib, "plugin_destroy");
        const dm_invoke_fn saved_fn = invoke_fn;
        void* const saved_handle = handle;
        Json ex_cfg{{"plugin_dir", out.u8string()}, {"simulate", true}, {"instruments", kv("digital", "dig1")},
                    {"options", ex_manifest["default_config"]["options"]}};
        handle = ex_create(ex_cfg.dump().c_str());
        invoke_fn = (dm_invoke_fn)sym(ex_lib, "plugin_invoke");
        CHECK(handle, "exported plugin opens its project");
        if (handle) {
            ex_set_host(handle, &g_host);
            CHECK(call("info").str("current") == "second", "exported plugin selects the pattern");
            CHECK(call("vectors")["rows"].items()[0].str("states")[0] == '1', "exported plugin has the saved edit");
            call("burst");
            wait_ms(50);
            CHECK(call("status").str("result") == "PASS", "exported plugin bursts");
            ex_destroy(handle);
        }
        invoke_fn = saved_fn;
        handle = saved_handle;
    }

    destroy(handle);

    std::printf("no instrument\n");    const Json cfg2{{"plugin_dir", src.u8string()}, {"instruments", kv("digital", nullptr)},
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
