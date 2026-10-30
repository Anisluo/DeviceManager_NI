#include "project.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <stdexcept>

namespace tdc {

namespace {

const double NaN = std::numeric_limits<double>::quiet_NaN();

std::string hex(unsigned v, int width)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", width, v);
    return buf;
}

char state_char(unsigned code)
{
    switch (code) {
    case 0: return '0'; case 1: return '1'; case 3: return 'L'; case 4: return 'H'; case 5: return 'X';
    case 6: return 'M'; case 7: return 'V'; case 8: return 'D'; case 9: return 'E';
    default: return '?';
    }
}

std::string lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// path has a component named ".cache" or "run" (scratch folders)
bool skipped(const std::string& path)
{
    std::string comp;
    for (char c : path + "/") {
        if (c == '/' || c == '\\') {
            if (comp == ".cache" || comp == "run") return true;
            comp.clear();
        } else {
            comp += c;
        }
    }
    return false;
}

} // namespace

Decoded decode(uint32_t ins)
{
    return Decoded{(ins >> 28) & 0xFu, (ins >> 23) & 0x1Fu, (ins >> 16) & 0x7Fu, ins & 0xFFFFu};
}

std::string op_name(unsigned op)
{
    switch (op) {
    case 0: return "";
    case 1: return "repeat";
    case 4: return "set_loop";
    case 5: return "end_loop";
    case 7: return "jump_if";
    case 15: return "halt";
    default: return "op" + hex(op, 1);
    }
}

// =========================================================================== project files

Pinmap load_pinmap(const std::string& path)
{
    Pinmap pm;
    xml_parse(read_file(path)).walk([&pm](const XmlNode& n) {
        if (n.tag == "NIDigitalPatternInstrument") pm.instruments.push_back(n.attr("name"));
        else if (n.tag == "DUTPin") pm.pins.push_back(n.attr("name"));
        else if (n.tag == "PinGroup") {
            std::vector<std::string> pins;
            for (auto& c : n.children) if (c.tag == "PinReference") pins.push_back(c.attr("pin"));
            pm.groups.emplace_back(n.attr("name"), pins);
        } else if (n.tag == "Connection") {
            pm.connections[n.attr("pin")] = Connection{n.attr("instrument"), std::atoi(n.attr("channel", "-1").c_str())};
        }
    });
    return pm;
}

std::vector<Spec> load_specs(const std::string& path)
{
    std::vector<Spec> out;
    const XmlNode root = xml_parse(read_file(path));
    for (auto& section : root.children) {
        if (section.tag != "Section") continue;
        for (auto& f : section.children) {
            if (f.tag != "Formula") continue;
            out.push_back(Spec{section.attr("name"), f.attr("symbol"), f.child_text("Definition"), f.child_text("Comment")});
        }
    }
    return out;
}

std::map<std::string, double> spec_symbols(const std::vector<Spec>& specs)
{
    std::map<std::string, double> sym;
    for (auto& s : specs) {
        try {
            sym[s.key()] = evaluate(s.definition, sym);
        } catch (const std::exception&) {
        }
    }
    return sym;
}

void write_specs(const std::string& src, const std::string& dst, const std::map<std::string, std::string>& values)
{
    // Textual edit keeps the editor's formatting and namespaces untouched.
    std::string text = read_file(src);
    for (auto& [key, value] : values) {
        const size_t dot = key.find('.');
        if (dot == std::string::npos) continue;
        const std::string section = key.substr(0, dot), symbol = key.substr(dot + 1);
        const size_t s0 = text.find("name=\"" + section + "\"");
        if (s0 == std::string::npos) continue;
        const size_t s1 = text.find("Section>", s0);
        const size_t f0 = text.find("symbol=\"" + symbol + "\"", s0);
        if (f0 == std::string::npos || (s1 != std::string::npos && f0 > s1)) continue;
        const size_t d0 = text.find("Definition>", f0);
        if (d0 == std::string::npos) continue;
        const size_t a = d0 + 11, b = text.find('<', a);
        if (b == std::string::npos) continue;
        text.replace(a, b - a, xml_escape(value));
    }
    write_file(dst, text);
}

std::vector<LevelRow> load_levels(const std::string& path)
{
    std::vector<LevelRow> out;
    xml_parse(read_file(path)).walk([&out](const XmlNode& n) {
        if (n.tag != "DigitalPinLevelSet") return;
        LevelRow row;
        row.pin = n.attr("pin");
        for (auto& c : n.children) row.fields[c.tag] = c.text;
        out.push_back(std::move(row));
    });
    return out;
}

std::vector<std::pair<std::string, Timeset>> load_timing(const std::string& path)
{
    std::vector<std::pair<std::string, Timeset>> out;
    xml_parse(read_file(path)).walk([&out](const XmlNode& n) {
        if (n.tag != "TimeSet") return;
        Timeset ts;
        ts.period_text = n.child_text("Period");
        double v = 0;
        if (parse_quantity(ts.period_text, v)) ts.period = v;
        out.emplace_back(n.attr("name"), ts);
    });
    return out;
}

// =========================================================================== pattern

Pattern::Pattern(const std::string& p) : path(p)
{
    file = stem(p);
    data = read_file(p);
    tdms = tdms::read(data);
    const tdms::Prop* np = tdms::find_prop(tdms.props, "name");
    name = np && !np->str.empty() ? np->str : file;
    auto strs = [this](const char* g, const char* c) {
        const tdms::Channel* ch = tdms.channel(g, c);
        return ch ? ch->strs : std::vector<std::string>{};
    };
    auto nums = [this](const char* g, const char* c) {
        const tdms::Channel* ch = tdms.channel(g, c);
        return ch ? ch->nums : std::vector<double>{};
    };
    pins = strs("PinItem", "PinName");
    timesets = strs("TimeSet", "Name");
    for (double v : nums("Pattern", "Instruction")) base_ins.push_back(uint32_t(v));
    comments = strs("Pattern", "Comment");
    label_names = strs("Pattern", "LocalLabelName");
    for (double v : nums("Pattern", "LocalLabelLocation")) label_locs.push_back(int(v));
    const size_t n = base_ins.size();
    comments.resize(std::max(comments.size(), n));
    const std::vector<double> raw = nums("Pattern", "PinState");
    const size_t width = (pins.size() + 1) / 2;
    for (size_t i = 0; i < n; ++i) {
        std::string s;
        for (size_t k = 0; k < pins.size(); ++k) {
            const size_t at = i * width + k / 2;
            const unsigned byte = at < raw.size() ? unsigned(raw[at]) : 0x55u;
            s += state_char((byte >> (4 * (k % 2))) & 0xFu);
        }
        states.push_back(s);
    }
    mtime = file_mtime(p);
    const std::string side = path_join(parent_dir(p), file + ".tdcgen.json");
    if (file_exists(side)) {
        try {
            meta = dm::Json::parse(read_file(side));
        } catch (const std::exception&) {
            meta = dm::Json();
        }
    }
    generated = meta.is_object() && meta.size() > 0;
}

std::vector<uint32_t> Pattern::ins() const
{
    std::vector<uint32_t> out = base_ins;
    for (auto& [i, v] : edits)
        if (i < out.size()) out[i] = (out[i] & ~0xFFFFu) | uint32_t(v);
    return out;
}

bool Pattern::modified() const
{
    for (auto& [i, v] : edits)
        if (i < base_ins.size() && decode(base_ins[i]).operand != unsigned(v)) return true;
    return false;
}

std::string Pattern::label_at(size_t i) const
{
    for (size_t k = 0; k < label_names.size() && k < label_locs.size(); ++k)
        if (size_t(label_locs[k]) == i) return label_names[k];
    return "";
}

std::string Pattern::section_of(size_t i) const
{
    std::string best;
    int where = -1;
    for (size_t k = 0; k < label_names.size() && k < label_locs.size(); ++k)
        if (where < label_locs[k] && size_t(label_locs[k]) <= i) { best = label_names[k]; where = label_locs[k]; }
    return best;
}

std::string Pattern::instruction_text(size_t i) const
{
    const Decoded d = decode(ins()[i]);
    auto label = [this](unsigned k) { return k < label_names.size() ? label_names[k] : "#" + std::to_string(k); };
    std::string text;
    switch (d.op) {
    case 0: break;
    case 1: case 4: text = op_name(d.op) + "(" + std::to_string(d.operand) + ")"; break;
    case 5: text = "end_loop(" + label(d.operand) + ")"; break;
    case 7: text = "jump_if(0x" + hex(d.flags, 2) + ((d.operand & 0x8000) ? "!" : "") + ", " + label(d.operand & 0x7FFF) + ")"; break;
    case 15: text = "halt"; break;
    default: text = op_name(d.op) + "(0x" + hex(d.operand, 4) + ")";
    }
    if (d.flags && d.op != 7) text = text.empty() ? "[flags 0x" + hex(d.flags, 2) + "]" : text + "  [flags 0x" + hex(d.flags, 2) + "]";
    return text;
}

int Pattern::loop_end(size_t i) const
{
    const std::vector<uint32_t> v = ins();
    int depth = 0;
    for (size_t j = i + 1; j < v.size(); ++j) {
        const unsigned op = decode(v[j]).op;
        if (op == 4) ++depth;
        else if (op == 5) {
            if (depth == 0) return int(j);
            --depth;
        }
    }
    return -1;
}

std::string Pattern::timeset_of(size_t i) const
{
    const unsigned k = decode(ins()[i]).timeset;
    return k < timesets.size() ? timesets[k] : "ts" + std::to_string(k);
}

std::pair<int, bool> Pattern::set_count(size_t i, long long value)
{
    if (i >= base_ins.size()) throw std::invalid_argument("向量 " + std::to_string(i) + " 超出范围");
    const Decoded d = decode(base_ins[i]);
    if (d.op != 1 && d.op != 4) throw std::invalid_argument("向量 " + std::to_string(i) + " 没有 repeat / set_loop 次数");
    const int clamped = int(std::min<long long>(std::max<long long>(value, 1), COUNT_MAX));
    if (unsigned(clamped) == d.operand) edits.erase(i);
    else edits[i] = clamped;
    return {clamped, clamped != value};
}

std::string Pattern::patched_bytes() const
{
    std::string out = data;
    const std::vector<uint32_t> v = ins();
    for (auto& [i, _] : edits) out = tdms::patch_u32(out, tdms, "Pattern", "Instruction", i, v[i]);
    return out;
}

RunResult Pattern::run(const std::map<std::string, double>& periods, double busy_delay, std::mt19937* rng,
                       double record_limit, long max_steps) const
{
    RunResult r;
    const std::vector<uint32_t> in = ins();
    const size_t n = in.size();
    std::map<unsigned, int> loc;
    for (size_t k = 0; k < label_locs.size(); ++k) loc[unsigned(k)] = label_locs[k];
    std::vector<Decoded> dec;
    std::vector<double> per;
    std::set<std::string> unknown, notes;
    for (uint32_t v : in) {
        const Decoded d = decode(v);
        dec.push_back(d);
        const std::string ts = d.timeset < timesets.size() ? timesets[d.timeset] : "ts" + std::to_string(d.timeset);
        auto it = periods.find(ts);
        if (it == periods.end() || it->second <= 0) { unknown.insert(ts); per.push_back(1e-6); }
        else per.push_back(it->second);
    }
    std::map<std::pair<size_t, size_t>, bool> edge_cache, program_cache;
    auto drive_edge = [&](size_t a, size_t b) {
        auto key = std::make_pair(a, b);
        auto it = edge_cache.find(key);
        if (it != edge_cache.end()) return it->second;
        bool e = false;
        for (size_t k = 0; k < states[a].size() && k < states[b].size(); ++k) {
            const char x = states[a][k], y = states[b][k];
            if ((x == '0' || x == '1') && (y == '0' || y == '1') && x != y) { e = true; break; }
        }
        return edge_cache[key] = e;
    };
    auto program_loop = [&](size_t lo, size_t hi) {
        auto key = std::make_pair(lo, hi);
        auto it = program_cache.find(key);
        if (it != program_cache.end()) return it->second;
        bool p = false;
        for (size_t j = lo; j < hi && j < n; ++j)
            if (dec[j].op == 4 || dec[j].op == 7) { p = true; break; }
        return program_cache[key] = p;
    };
    std::uniform_real_distribution<double> jitter(0.8, 1.2);
    auto delay = [&]() { return busy_delay * (rng ? jitter(*rng) : 1.0); };

    struct Loop { long count; double t_iter; bool first; int depth; size_t pc; };
    std::vector<Loop> stack;
    double t = 0, last_drive_edge = 0;
    double ready_at = delay();          // BUSY high from here on; NaN = converting
    bool recording = true;
    size_t pc = 0;
    long prev_pc = -1;
    while (pc < n) {
        if (++r.steps > max_steps) {
            notes.insert("步数超过上限，结果为估算");
            r.extrapolated = true;
            break;
        }
        const Decoded& d = dec[pc];
        const double period = per[pc];
        const std::string& st = states[pc];
        const bool changed = prev_pc < 0 || states[size_t(prev_pc)] != st;
        if (prev_pc >= 0 && changed && drive_edge(size_t(prev_pc), pc)) last_drive_edge = t;
        if (recording && changed) r.timeline.emplace_back(t, st);
        prev_pc = long(pc);

        switch (d.op) {
        case 1:                                                   // repeat
            t += period * std::max(1u, d.operand);
            ++pc;
            break;
        case 4:                                                   // set_loop
            t += period;
            stack.push_back(Loop{long(std::max(1u, d.operand)), t, true, int(stack.size()) + 1, pc});
            ++pc;
            break;
        case 5: {                                                 // end_loop
            t += period;
            if (stack.empty()) {
                notes.insert("v" + std::to_string(pc) + ": end_loop 没有对应的 set_loop");
                ++pc;
                break;
            }
            Loop& top = stack.back();
            const long remaining = top.count - 1;
            const unsigned li = d.operand & 0x7FFF;
            const size_t target = loc.count(li) ? size_t(loc[li]) : pc + 1;
            if (target <= top.pc)
                notes.insert("v" + std::to_string(pc) + ": end_loop(" + (li < label_names.size() ? label_names[li] : std::to_string(li)) +
                             ") 跳回到 v" + std::to_string(top.pc) + " 的 set_loop 之前，循环体可能写错了标签");
            if (top.depth == 1 && top.first && program_loop(target, pc)) {
                r.outer_iteration = t - top.t_iter;
                r.outer_loops = int(top.count);
                r.outer_set_loop = int(top.pc);
                if (recording) {
                    recording = false;
                    r.timeline.emplace_back(t, st);
                }
            }
            top.first = false;
            if (remaining > 1 && !recording) {
                t += (t - top.t_iter) * double(remaining);
                r.extrapolated = true;
                stack.pop_back();
                ++pc;
            } else if (remaining > 0) {
                top.count = remaining;
                top.t_iter = t;
                pc = target;
            } else {
                stack.pop_back();
                ++pc;
            }
            break;
        }
        case 7: {                                                 // jump_if: wait on a compared pin
            const unsigned li = d.operand & 0x7FFF;
            const long target = loc.count(li) ? loc[li] : -1;
            const bool high = st.find('H') != std::string::npos, low = st.find('L') != std::string::npos;
            if (target == long(pc) && high) {
                if (std::isnan(ready_at)) ready_at = last_drive_edge + delay();
                const double cycles = std::max(1.0, std::ceil(std::max(0.0, ready_at - t) / period));
                if (recording) r.waits.emplace_back(t, cycles * period);
                t += cycles * period;
                ready_at = NaN;
            } else {
                if (target != long(pc)) notes.insert("v" + std::to_string(pc) + ": jump_if 到其他标签，仿真按条件不成立处理");
                (void)low;                                         // waiting for low: the TDC holds BUSY low while converting
                t += period;
            }
            ++pc;
            break;
        }
        case 15:                                                  // halt
            t += period;
            pc = n;
            break;
        default:
            if (d.op != 0) notes.insert("v" + std::to_string(pc) + ": 未识别的指令 op" + hex(d.op, 1) + "，按普通向量执行");
            t += period;
            ++pc;
        }
        if (recording && (t > record_limit || r.timeline.size() > 20000)) {
            recording = false;
            r.timeline.emplace_back(t, st);
        }
    }
    for (auto& ts : unknown) notes.insert("未知时序 " + ts + "，按 1 µs 计");
    if (recording || r.timeline.empty())
        r.timeline.emplace_back(t, prev_pc >= 0 ? states[size_t(prev_pc)] : std::string());
    r.total = t;
    r.notes.assign(notes.begin(), notes.end());
    return r;
}

// =========================================================================== analysis

namespace {

struct Edges { std::vector<double> rises, falls; };

Edges pulses(const std::vector<std::pair<double, std::string>>& tl, size_t k)
{
    Edges e;
    char prev = 0;
    for (auto& [t, s] : tl) {
        const char c = k < s.size() ? s[k] : 0;
        if (prev == '0' && c == '1') e.rises.push_back(t);
        else if (prev == '1' && c == '0') e.falls.push_back(t);
        if (c == '0' || c == '1') prev = c;
    }
    return e;
}

std::string gen_role(const std::string& role)
{
    if (role == "TRIG") return "BUSY";
    if (role == "OUT1") return "START";
    if (role == "OUT2") return "HIT";
    return role;
}

double level_of(char c)
{
    switch (c) {
    case '0': case 'L': return 0.0;
    case '1': case 'H': return 1.0;
    default: return 0.5;
    }
}

dm::Json num(double v) { return std::isfinite(v) ? dm::Json(v) : dm::Json(); }

} // namespace

dm::Json analyze(const Pattern& p, const std::map<std::string, double>& periods, double busy_delay)
{
    const RunResult r = p.run(periods, busy_delay);
    const auto& tl = r.timeline;
    std::vector<std::pair<std::string, std::string>> roles;     // pin -> role, pin order
    auto role_of = [&roles](const std::string& pin) -> std::string {
        for (auto& [p2, r2] : roles) if (p2 == pin) return r2;
        return "";
    };
    std::map<std::string, Edges> edges;
    std::map<std::string, std::string> given;
    if (p.meta.is_object())
        for (auto& [role, pin] : p.meta["roles"].members())
            if (pin.is_string() && !pin.as_string().empty()) given[pin.as_string()] = role;
    for (size_t k = 0; k < p.pins.size(); ++k) {
        edges[p.pins[k]] = pulses(tl, k);
        if (!given.empty()) continue;
        bool compared = false;
        for (auto& [t, s] : tl) if (k < s.size() && (s[k] == 'H' || s[k] == 'L')) { compared = true; break; }
        if (compared) roles.emplace_back(p.pins[k], "BUSY");
    }
    if (!given.empty()) {
        for (auto& pin : p.pins) if (given.count(pin)) roles.emplace_back(pin, given[pin]);
    } else {
        // START rises first (ties: fewer pulses); HIT is the busiest of the others; a lone pin
        // pulsing more than once is a HIT train.
        std::vector<std::string> drive;
        for (auto& pin : p.pins) if (!edges[pin].rises.empty() && role_of(pin).empty()) drive.push_back(pin);
        std::stable_sort(drive.begin(), drive.end(), [&edges](const std::string& a, const std::string& b) {
            const auto &ea = edges[a].rises, &eb = edges[b].rises;
            return ea[0] != eb[0] ? ea[0] < eb[0] : ea.size() < eb.size();
        });
        if (drive.size() == 1 && edges[drive[0]].rises.size() > 1) {
            roles.emplace_back(drive[0], "HIT");
        } else if (!drive.empty()) {
            roles.emplace_back(drive[0], "START");
            std::vector<std::string> rest(drive.begin() + 1, drive.end());
            std::stable_sort(rest.begin(), rest.end(), [&edges](const std::string& a, const std::string& b) {
                return edges[a].rises.size() < edges[b].rises.size();
            });
            if (!rest.empty()) roles.emplace_back(rest.back(), "HIT");
            for (size_t k = 0; k + 1 < rest.size(); ++k) roles.emplace_back(rest[k], "DRIVE");
        }
    }
    auto pin_with = [&roles](const char* want) -> std::string {
        for (auto& [pin, role] : roles) if (gen_role(role) == want) return pin;
        return "";
    };
    const std::string start = pin_with("START"), hit = pin_with("HIT"), busy = pin_with("BUSY");
    auto fall_after = [&edges](const std::string& pin, double t) {
        const auto& falls = edges[pin].falls;
        auto it = std::upper_bound(falls.begin(), falls.end(), t);
        return it == falls.end() ? NaN : *it - t;
    };
    auto width = [&](const std::string& pin) {
        double sum = 0;
        int n = 0;
        for (double r0 : edges[pin].rises) { const double w = fall_after(pin, r0); if (std::isfinite(w)) { sum += w; ++n; } }
        return n ? sum / n : NaN;
    };
    auto mean_period = [](const std::vector<double>& rises) {
        return rises.size() > 1 ? (rises.back() - rises.front()) / double(rises.size() - 1) : NaN;
    };
    const std::vector<double> hit_rises = hit.empty() ? std::vector<double>{} : edges[hit].rises;
    const std::vector<double> start_rises = start.empty() ? std::vector<double>{} : edges[start].rises;
    const double start_rise = start_rises.empty() ? NaN : start_rises[0];
    double busy_wait = NaN;
    if (!r.waits.empty()) { busy_wait = 0; for (auto& w : r.waits) busy_wait += w.second; }

    dm::Json role_obj = dm::Json::object(), pins = dm::Json::array();
    for (auto& [pin, role] : roles) role_obj[pin] = role;
    for (auto& pin : p.pins) pins.push_back(pin);
    const char* dash = "\xE2\x80\x94";
    dm::Json out{{"pattern", p.file}, {"label", p.name}, {"pins", pins}, {"roles", role_obj},
                 {"start_pin", start.empty() ? dash : start}, {"hit_pin", hit.empty() ? dash : hit},
                 {"busy_pin", busy.empty() ? dash : busy},
                 {"start_width", num(start.empty() ? NaN : width(start))},
                 {"start_count", int(start_rises.size())}, {"start_period", num(mean_period(start_rises))},
                 {"generated", p.generated}, {"hit_count", int(hit_rises.size())},
                 {"hit_period", num(mean_period(hit_rises))}, {"hit_width", num(hit.empty() ? NaN : width(hit))},
                 {"start_to_hit", num(!hit_rises.empty() && std::isfinite(start_rise) ? hit_rises[0] - start_rise : NaN)},
                 {"busy_wait", num(busy_wait)},
                 {"iteration", r.outer_iteration >= 0 ? r.outer_iteration : r.total},
                 {"loops", r.outer_loops > 0 ? r.outer_loops : 1},
                 {"outer_set_loop", r.outer_set_loop >= 0 ? dm::Json(r.outer_set_loop) : dm::Json()},
                 {"total", r.total}, {"extrapolated", r.extrapolated}, {"modified", p.modified()}};
    dm::Json notes = dm::Json::array();
    for (auto& n : r.notes) notes.push_back(n);
    out["notes"] = notes;

    struct Ev { std::string event, pin; double t, rel, width; };
    std::vector<Ev> ev;
    if (std::isfinite(start_rise)) {
        const std::string label = p.generated ? "OUT1" : "START";
        for (size_t i = 0; i < start_rises.size() && i < 200; ++i)
            ev.push_back(Ev{start_rises.size() == 1 ? label : label + " " + std::to_string(i + 1), start, start_rises[i],
                            start_rises[i] - start_rise, fall_after(start, start_rises[i])});
    }
    const double t0 = std::isfinite(start_rise) ? start_rise : (hit_rises.empty() ? 0.0 : hit_rises[0]);
    for (size_t i = 0; i < hit_rises.size(); ++i)
        ev.push_back(Ev{std::string(p.generated ? "OUT2 " : "HIT ") + std::to_string(i + 1), hit, hit_rises[i],
                        hit_rises[i] - t0, fall_after(hit, hit_rises[i])});
    for (auto& [t, w] : r.waits)
        ev.push_back(Ev{p.generated ? "等待 TRIG 边沿" : "等待 BUSY", busy, t, std::isfinite(start_rise) ? t - t0 : NaN, w});
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t < b.t; });
    dm::Json events = dm::Json::array();
    for (auto& e : ev)
        events.push_back(dm::Json{{"event", e.event}, {"pin", e.pin.empty() ? dm::Json() : dm::Json(e.pin)},
                                  {"t", e.t}, {"rel", num(e.rel)}, {"width", num(e.width)}});
    out["events"] = events;

    // timeline: stacked digital traces {t: [ms], channels: [{name, unit, data}]}
    double resolution = NaN;
    for (size_t i = 0; i < p.base_ins.size(); ++i) {
        auto it = periods.find(p.timeset_of(i));
        if (it != periods.end() && it->second > 0 && !(resolution <= it->second)) resolution = it->second;
    }
    double clock = NaN;
    if (p.meta.is_object() && p.meta["summary"].contains("clock_mhz")) clock = p.meta["summary"]["clock_mhz"].as_number();
    const size_t np = p.pins.size();
    std::vector<dm::Json> chans(np, dm::Json::array());
    dm::Json ts = dm::Json::array();
    const std::string* prev = nullptr;
    auto push = [&](double t, const std::string& s) {
        ts.push_back(t * 1e3);
        for (size_t k = 0; k < np; ++k)
            chans[k].push_back(level_of(k < s.size() ? s[k] : 'X') * 0.8 + 1.2 * double(np - 1 - k));
    };
    for (auto& [t, s] : tl) {
        if (prev) push(t, *prev);
        push(t, s);
        prev = &s;
    }
    dm::Json channels = dm::Json::array();
    for (size_t k = 0; k < np; ++k) {
        const std::string role = role_of(p.pins[k]);
        std::string name = p.pins[k];
        if (role == "CLK" && std::isfinite(clock)) name += " (CLK " + fmt_g(clock, 6) + " MHz)";
        else if (!role.empty()) name += " (" + role + ")";
        channels.push_back(dm::Json{{"name", name}, {"unit", ""}, {"data", chans[k]}});
    }
    out["timeline"] = dm::Json{{"t", ts}, {"dt_ms", std::isfinite(resolution) ? resolution * 1e3 : 0.0}, {"channels", channels}};
    return out;
}

// =========================================================================== project

Project::Project(const std::string& f, std::vector<std::string> extra) : folder(f), extra_dirs(std::move(extra))
{
    auto first = [this](const char* ext) {
        for (auto& p : find_files(folder, ext)) if (!skipped(p)) return p;
        return std::string();
    };
    pinmap_path = first(".pinmap");
    specs_path = first(".specs");
    levels_path = first(".digilevels");
    timing_path = first(".digitiming");
    if (pinmap_path.empty()) throw std::runtime_error(folder + "：没有 .pinmap");
    pinmap = load_pinmap(pinmap_path);
    if (!specs_path.empty()) specs = load_specs(specs_path);
    if (!levels_path.empty()) levels = load_levels(levels_path);
    rescan();
}

std::vector<std::string> Project::timing_files() const
{
    std::vector<std::string> out;
    std::vector<std::string> roots{folder};
    roots.insert(roots.end(), extra_dirs.begin(), extra_dirs.end());
    for (auto& root : roots)
        for (auto& p : find_files(root, ".digitiming"))
            if (!skipped(p) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    // the project's own sheet first
    auto it = std::find(out.begin(), out.end(), timing_path);
    if (it != out.end()) std::rotate(out.begin(), it, it + 1);
    return out;
}

void Project::rescan()
{
    timing.clear();
    timing_file.clear();
    for (auto& path : timing_files()) {
        for (auto& [name, ts] : load_timing(path)) {
            if (timing_file.count(name)) continue;
            timing.emplace_back(name, ts);
            timing_file[name] = path;
        }
    }
    std::vector<std::string> roots{folder};
    roots.insert(roots.end(), extra_dirs.begin(), extra_dirs.end());
    std::vector<std::shared_ptr<Pattern>> found;
    for (auto& root : roots) {
        for (auto& path : find_files(root, ".digipat")) {
            if (skipped(path)) continue;
            const std::string name = stem(path);
            bool dup = false;
            for (auto& f2 : found) if (f2->file == name) dup = true;
            if (dup) continue;
            std::shared_ptr<Pattern> old = find(name);
            if (old && old->path == path && old->mtime == file_mtime(path)) found.push_back(old);
            else found.push_back(std::make_shared<Pattern>(path));
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return lower(a->file) < lower(b->file); });
    patterns = std::move(found);
}

std::shared_ptr<Pattern> Project::find(const std::string& name) const
{
    for (auto& p : patterns) if (p->file == name) return p;
    return nullptr;
}

std::map<std::string, double> Project::periods() const
{
    std::map<std::string, double> out;
    for (auto& [name, ts] : timing) if (ts.period > 0) out[name] = ts.period;
    return out;
}

} // namespace tdc
