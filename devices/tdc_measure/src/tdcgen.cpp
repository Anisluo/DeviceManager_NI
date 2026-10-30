#include "tdcgen.hpp"
#include "project.hpp"
#include "tdms.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace tdc {

namespace {

constexpr long MAX_REPEAT = 0xFFFF;
constexpr size_t MAX_VECTORS = 200000;
// instruction words copied from the DemoPrj-TDC wait loop (WaitForBusy): a compare vector with
// flags 0x20, then jump_if(flags 0x28, operand 0x8000 | label) on itself
constexpr unsigned PRE_WAIT_FLAGS = 0x20, WAIT_FLAGS = 0x28;

uint32_t ins(unsigned op, unsigned operand = 0, unsigned flags = 0, unsigned ts = 0)
{
    return (uint32_t(op) << 28) | (uint32_t(ts) << 23) | (uint32_t(flags) << 16) | (operand & 0xFFFFu);
}

double getnum(const dm::Json& p, const char* k) { return p[k].is_string() ? std::atof(p[k].as_string().c_str()) : p[k].as_number(); }
std::string getstr(const dm::Json& p, const char* k) { return p[k].is_string() ? p[k].as_string() : std::string(); }

std::string fmt_s(double s)
{
    if (s >= 1e-6) return fmt_g(s / 1e-6, 6) + " \xC2\xB5s";
    return fmt_g(s / 1e-9, 6) + " ns";
}

} // namespace

dm::Json gen_defaults()
{
    return dm::Json{{"name", "TDC_GEN"}, {"clk_pin", "I3"}, {"trig_pin", "I0"}, {"out1_pin", "I1"}, {"out2_pin", "I2"},
                    {"clock_mhz", 50.0}, {"edge", "rising"},
                    {"out1_khz", 250.0}, {"out1_cycles", 1}, {"out1_delay_us", 0.0},
                    {"out2_khz", 500.0}, {"out2_cycles", 16}, {"out2_delay_us", 200.0},
                    {"post_us", 1000.0}, {"loops", 1}};
}

GenPlan gen_plan(const dm::Json& params, const std::vector<std::string>& pins_available)
{
    dm::Json p = gen_defaults();
    for (auto& [k, v] : params.members())
        if (!v.is_null() && !(v.is_string() && v.as_string().empty() && k != "clk_pin")) p[k] = v;
    GenPlan pl;
    pl.params = p;
    pl.name = getstr(p, "name");
    while (!pl.name.empty() && pl.name.back() == ' ') pl.name.pop_back();
    while (!pl.name.empty() && pl.name.front() == ' ') pl.name.erase(pl.name.begin());
    if (!valid_name(pl.name))
        throw GenError("图形名 '" + pl.name + "' 无效：只能用字母、数字和下划线，且不能以数字开头");
    pl.clk = getstr(p, "clk_pin");
    pl.trig = getstr(p, "trig_pin");
    pl.out1 = getstr(p, "out1_pin");
    pl.out2 = getstr(p, "out2_pin");
    const std::vector<std::pair<const char*, std::string>> roles{{"CLK", pl.clk}, {"TRIG", pl.trig}, {"OUT1", pl.out1}, {"OUT2", pl.out2}};
    std::set<std::string> seen;
    size_t used = 0;
    for (auto& [role, pin] : roles) {
        if (pin.empty()) continue;
        if (std::find(pins_available.begin(), pins_available.end(), pin) == pins_available.end())
            throw GenError(std::string(role) + " 引脚 " + pin + " 不在引脚映射里");
        seen.insert(pin);
        ++used;
    }
    if (seen.size() != used) throw GenError("四个功能不能用同一个引脚");
    if (pl.trig.empty() || pl.out1.empty() || pl.out2.empty()) throw GenError("TRIG、OUT1、OUT2 必须指定引脚");
    const double clock = getnum(p, "clock_mhz") * 1e6;
    if (!(clock >= 25e3 && clock <= 200e6)) throw GenError("时钟频率应在 0.025 … 200 MHz（6571 周期 5 ns … 40 µs）");
    const double T = 1.0 / clock;
    pl.period = T;
    // one timeset per clock period and CLK / TRIG pin choice, so sheets never disagree on a name
    pl.timeset = "tdcgen_" + std::to_string(std::llround(T * 1e12)) + "ps_" + (pl.clk.empty() ? "noclk" : pl.clk) + "_" + pl.trig;
    for (int k = 1; k <= 2; ++k) {
        const std::string key = "out" + std::to_string(k);
        const double f = getnum(p, (key + "_khz").c_str()) * 1e3;
        const long n = long(std::llround(getnum(p, (key + "_cycles").c_str())));
        if (!(f > 0) || n < 1) throw GenError("OUT" + std::to_string(k) + " 的频率和周期数必须大于 0");
        Wave w;
        w.half = std::max(1L, long(std::llround(1.0 / (2 * f) / T)));
        w.delay = std::max(0L, long(std::llround(getnum(p, (key + "_delay_us").c_str()) * 1e-6 / T)));
        w.cycles = n;
        w.freq = 1.0 / (2.0 * double(w.half) * T);
        w.start = double(w.delay) * T;
        w.end = double(w.delay + 2 * w.half * n) * T;
        if (std::fabs(w.freq - f) / f > 1e-6)
            pl.warnings.push_back("OUT" + std::to_string(k) + " 频率按时钟周期取整为 " + fmt_g(w.freq / 1e3, 6) +
                                  " kHz（要求 " + fmt_g(f / 1e3, 6) + " kHz）");
        (k == 1 ? pl.w1 : pl.w2) = w;
    }
    pl.post = std::max(1L, long(std::llround(getnum(p, "post_us") * 1e-6 / T)));
    const double loops = getnum(p, "loops");
    if (!(loops >= 1 && loops <= MAX_REPEAT)) throw GenError("循环次数应在 1 … " + std::to_string(MAX_REPEAT));
    pl.loops = int(std::llround(loops));
    pl.edge = getstr(p, "edge").rfind("f", 0) == 0 ? "falling" : "rising";
    return pl;
}

std::vector<std::string> gen_pins(const GenPlan& pl)
{
    std::vector<std::string> pins;
    for (const std::string* p : {&pl.clk, &pl.trig, &pl.out1, &pl.out2})
        if (!p->empty()) pins.push_back(*p);
    return pins;
}

std::vector<GenVector> gen_vectors(const GenPlan& pl)
{
    const std::vector<std::string> pins = gen_pins(pl);
    auto at = [&pins](const std::string& pin) { return size_t(std::find(pins.begin(), pins.end(), pin) - pins.begin()); };
    std::string idle(pins.size(), '0');
    idle[at(pl.trig)] = 'X';
    if (!pl.clk.empty()) idle[at(pl.clk)] = '1';                // return-to-low: one pulse per cycle

    struct Raw { std::string label, states; bool fill = false; uint32_t ins = 0; std::string comment; };
    std::vector<Raw> vec;
    auto add = [&](const std::string& label, std::string states, bool fill, uint32_t in, const std::string& comment) {
        vec.push_back(Raw{label, std::move(states), fill, in, comment});
    };
    auto with = [&](std::initializer_list<std::pair<std::string, char>> set) {
        std::string s = idle;
        for (auto& [pin, c] : set) s[at(pin)] = c;
        return s;
    };
    auto hold = [&](const std::string& states, long cycles, const std::string& comment) {
        bool first = true;
        while (cycles > 0) {
            const long n = std::min(cycles, MAX_REPEAT);
            add("", states, false, n > 1 ? ins(1, unsigned(n)) : ins(0), first ? comment : "");
            first = false;
            cycles -= n;
        }
    };

    add(pl.name, idle, false, ins(0), "由 TDC测量 插件生成");
    const bool loop = pl.loops > 1;
    if (loop) add("", idle, false, ins(4, unsigned(pl.loops)), "重复 " + std::to_string(pl.loops) + " 次测量");
    const char first = pl.edge == "rising" ? 'L' : 'H', second = pl.edge == "rising" ? 'H' : 'L';
    add("Measure", with({{pl.trig, first}}), false, ins(0, 0, PRE_WAIT_FLAGS), std::string("TRIG 先等") + (first == 'L' ? "低" : "高") + "电平");
    add("WaitFirst", with({{pl.trig, first}}), true, 0, "");
    add("", with({{pl.trig, second}}), false, ins(0, 0, PRE_WAIT_FLAGS), pl.edge == "rising" ? "检测上升沿" : "检测下降沿");
    add("WaitEdge", with({{pl.trig, second}}), true, 0, "边沿到来后开始输出");

    auto edges = [](const Wave& w) {
        std::vector<long> e;
        for (long k = 0; k <= 2 * w.cycles; ++k) e.push_back(w.delay + k * w.half);
        return e;
    };
    const std::vector<long> e1 = edges(pl.w1), e2 = edges(pl.w2);
    std::set<long> markset{0};
    markset.insert(e1.begin(), e1.end());
    markset.insert(e2.begin(), e2.end());
    const std::vector<long> marks(markset.begin(), markset.end());
    auto level = [](const std::vector<long>& es, long t) {
        if (t < es.front() || t >= es.back()) return '0';
        return ((t - es[0]) / (es[1] - es[0])) % 2 == 0 ? '1' : '0';
    };
    for (size_t i = 0; i + 1 < marks.size(); ++i) {
        const long a = marks[i], b = marks[i + 1];
        std::string comment;
        if (a == e1[0]) comment = "OUT1 开始：" + fmt_g(pl.w1.freq / 1e3, 6) + " kHz × " + std::to_string(pl.w1.cycles);
        if (a == e2[0]) comment += (comment.empty() ? "" : "；") + std::string("OUT2 开始：") + fmt_g(pl.w2.freq / 1e3, 6) +
                                   " kHz × " + std::to_string(pl.w2.cycles);
        std::string s = idle;
        s[at(pl.out1)] = level(e1, a);
        s[at(pl.out2)] = level(e2, a);
        hold(s, b - a, comment);
    }
    if (marks.back() == 0) hold(idle, 1, "");
    hold(idle, pl.post, "输出结束后等待");
    if (loop) add("", idle, true, 0, "");                      // end_loop(Measure), filled below
    add("", idle, false, ins(15, 0xF), "结束");
    if (vec.size() > MAX_VECTORS)
        throw GenError("向量数 " + std::to_string(vec.size()) + " 超过 " + std::to_string(MAX_VECTORS) + "，请减少周期数或降低时钟频率");

    std::map<std::string, unsigned> index;
    for (auto& v : vec) if (!v.label.empty()) { const unsigned k = unsigned(index.size()); index[v.label] = k; }
    std::vector<GenVector> out;
    for (auto& v : vec) {
        uint32_t in = v.ins;
        if (v.fill) in = (v.label == "WaitFirst" || v.label == "WaitEdge") ? ins(7, 0x8000u | index[v.label], WAIT_FLAGS)
                                                                         : ins(5, index["Measure"]);
        out.push_back(GenVector{v.label, v.states, in, v.comment});
    }
    return out;
}

std::string gen_build(const GenPlan& pl, const std::string& template_bytes)
{
    const std::vector<std::string> pins = gen_pins(pl);
    const std::vector<GenVector> vec = gen_vectors(pl);
    tdms::File f = tdms::read(template_bytes);
    auto setc = [&f](const char* g, const char* c) -> tdms::Channel& {
        tdms::Channel* ch = f.channel(g, c);
        if (!ch) throw GenError(std::string("模板图形缺少 ") + g + "/" + c);
        ch->nums.clear();
        ch->strs.clear();
        return *ch;
    };
    std::vector<double> packed;
    for (auto& v : vec) {
        std::vector<unsigned> row((pins.size() + 1) / 2, 0);
        for (size_t k = 0; k < pins.size(); ++k) row[k / 2] |= state_code(v.states[k]) << (4 * (k % 2));
        packed.insert(packed.end(), row.begin(), row.end());
    }
    tdms::set_prop(f.props, "name", pl.name);
    for (const char* k : {"description", "title", "author"})
        if (tdms::find_prop(f.props, k)) tdms::set_prop(f.props, k, std::string());
    setc("Header", "Comment");
    setc("TimeSet", "Name").strs = {pl.timeset};
    setc("Import", "Name");
    setc("PinItem", "PinGroup");
    setc("PinItem", "PinGroupFormat");
    setc("PinItem", "PinName").strs = pins;
    setc("PinItem", "PinNameGroupIndex").nums.assign(pins.size(), -1);
    auto& comments = setc("Pattern", "Comment");
    auto& names = setc("Pattern", "LocalLabelName");
    auto& locs = setc("Pattern", "LocalLabelLocation");
    auto& refd = setc("Pattern", "LocalLabelReferenced");
    for (size_t i = 0; i < vec.size(); ++i) {
        comments.strs.push_back(vec[i].comment);
        if (vec[i].label.empty()) continue;
        names.strs.push_back(vec[i].label);
        locs.nums.push_back(double(i));
        refd.nums.push_back(vec[i].label == "WaitFirst" || vec[i].label == "WaitEdge" || (vec[i].label == "Measure" && pl.loops > 1));
    }
    setc("Pattern", "ExportLabelName");
    setc("Pattern", "ExportLabelLocation");
    setc("Pattern", "PinState").nums = packed;
    auto& in = setc("Pattern", "Instruction");
    for (auto& v : vec) in.nums.push_back(double(v.ins));
    tdms::set_prop(f.group("Pattern")->props, "VectorCount", double(vec.size()));
    return tdms::write(f);
}

std::string gen_timing_sheet(const GenPlan& pl, const std::vector<std::string>& all_pins, const std::string& folder)
{
    const std::string path = path_join(folder, pl.timeset + ".digitiming");
    const double T = pl.period;
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<TimingFile xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                    "schemaVersion=\"1.0\" xmlns=\"http://www.ni.com/Semiconductor/Timing\">\n"
                    "  <TimingSheet>\n    <TimeSets>\n      <TimeSet name=\"" + pl.timeset + "\">\n"
                    "        <Period>" + fmt_s(T) + "</Period>\n        <PinEdges>\n";
    for (auto& pin : all_pins) {
        x += "          <PinEdge pin=\"" + xml_escape(pin) + "\">\n";
        if (pin == pl.clk)                                 // element and edge order as the editor saves RL
            x += "            <ReturnToLow>\n              <On>0</On>\n              <Data>0</Data>\n"
                 "              <Return>" + fmt_s(T / 2) + "</Return>\n              <Off>0</Off>\n            </ReturnToLow>\n";
        else
            x += "            <DriveNonReturn>\n              <On>0</On>\n              <Data>0</Data>\n"
                 "              <Off>0</Off>\n            </DriveNonReturn>\n";
        x += "            <CompareStrobe>\n              <Strobe>" + (pin == pl.trig ? fmt_s(T / 2) : std::string("0")) +
             "</Strobe>\n            </CompareStrobe>\n            <DataSource>Pattern</DataSource>\n          </PinEdge>\n";
    }
    x += "        </PinEdges>\n      </TimeSet>\n    </TimeSets>\n  </TimingSheet>\n</TimingFile>\n";
    write_file(path, x);
    return path;
}

dm::Json gen_summary(const GenPlan& pl)
{
    const double T = pl.period;
    const double out_end = std::max(pl.w1.end, pl.w2.end);
    dm::Json warnings = dm::Json::array();
    for (auto& w : pl.warnings) warnings.push_back(w);
    return dm::Json{{"clock_mhz", 1e-6 / T}, {"period", T}, {"timeset", pl.timeset},
                    {"out1_khz", pl.w1.freq / 1e3}, {"out2_khz", pl.w2.freq / 1e3},
                    {"out1_start", pl.w1.start}, {"out2_start", pl.w2.start}, {"out1_to_out2", pl.w2.start - pl.w1.start},
                    {"output_time", out_end}, {"measure", out_end + double(pl.post) * T}, {"loops", pl.loops},
                    {"warnings", warnings}};
}

std::string gen_note(const GenPlan& pl)
{
    std::string s;
    for (auto& w : pl.warnings) s += (s.empty() ? "" : "；") + w;
    return s.empty() ? "无（频率、延时都是时钟周期的整数倍）" : s;
}

} // namespace tdc
