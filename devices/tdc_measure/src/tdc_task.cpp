// tdc_task.cpp — TDC测量: a test task (one DLL) that runs TDC stimulus patterns on a PXIe-6571.
//
// The task owns the Digital Pattern Editor project (DemoPrj-TDC), decodes and analyses its
// patterns, edits repeat / set_loop counts, generates four-pin stimulus patterns (CLK / TRIG /
// OUT1 / OUT2), and bursts them on the 6571 the host detected. It never opens the instrument
// itself: every instrument action is a host call to that device ("pattern_project",
// "pattern_timing", "pattern_specs", "pattern_start", "pattern_status", "pattern_abort").
#include "project.hpp"
#include "tdcgen.hpp"
#include "util.hpp"

#include <dm/plugin.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <random>

using dm::Json;
using dm::PluginError;
using Clock = std::chrono::steady_clock;

static const char* kManifest = R"({
  "id": "tdc_measure",
  "name": "TDC测量",
  "version": "0.2.0",
  "type": "cpp",
  "abi": 1,
  "category": "task",
  "requires": {"digital": {"models": ["PXIe-6571"]}}
})";

// One-member object. (kv("k", "v") with two string literals would pick std::string's
// iterator-range constructor.)
static Json kv(const char* key, Json value)
{
    Json j = Json::object();
    j[key] = std::move(value);
    return j;
}

namespace {

const double NaN = std::nan("");

std::string state_text(const std::string& s)
{
    if (s == "running") return "运行中";
    if (s == "done") return "完成";
    if (s == "aborted") return "已中止";
    if (s == "error") return "错误";
    return "空闲";
}

double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

} // namespace

class TdcMeasure : public dm::Plugin {
public:
    explicit TdcMeasure(const Json& cfg)
    {
        const Json& o = cfg["options"];
        plugin_dir_ = cfg.str("plugin_dir", ".");
        std::string folder = o.str("project", "project/DemoPrj-TDC");
        if (!(folder.size() > 1 && (folder[1] == ':' || folder[0] == '/' || folder[0] == '\\')))
            folder = tdc::path_join(plugin_dir_, folder);
        work_ = o.str("work_dir", tdc::path_join(plugin_dir_, "_work"));
        gen_dir_ = tdc::path_join(work_, "gen");
        tdc::make_dirs(gen_dir_);
        project_ = std::make_unique<tdc::Project>(folder, std::vector<std::string>{gen_dir_});
        busy_delay_ = o.number("busy_delay_us", 120.0) * 1e-6;
        digital_ = cfg["instruments"]["digital"].as_string();
        simulate_ = cfg.boolean("simulate", true);
        if (project_->patterns.empty()) throw std::runtime_error(folder + "：没有 .digipat 图形");
        const std::string want = o.str("pattern", "TDC_M20_2");
        current_ = project_->find(want) ? want : project_->patterns.front()->file;
        gen_template_ = tdc::path_join(project_->folder, "Patterns/TDC_M20_2.digipat");
        rng_.seed(std::random_device{}());
        register_methods();
    }

private:
    // ------------------------------------------------------------------ helpers
    tdc::Pattern& pattern(const std::string& name = {})
    {
        auto p = project_->find(name.empty() ? current_ : name);
        if (!p) throw PluginError(DM_E_BAD_ARGS, "没有图形 " + (name.empty() ? current_ : name));
        return *p;
    }

    const Json& analysis(const std::string& name = {})
    {
        tdc::Pattern& p = pattern(name);
        std::string key = p.file + "|" + std::to_string(p.mtime) + "|" + std::to_string(busy_delay_);
        for (auto& [i, v] : p.edits) key += "|" + std::to_string(i) + "=" + std::to_string(v);
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            if (cache_.size() > 64) cache_.clear();
            it = cache_.emplace(key, tdc::analyze(p, project_->periods(), busy_delay_)).first;
        }
        return it->second;
    }

    std::vector<tdc::Spec> specs_now() const
    {
        std::vector<tdc::Spec> specs = project_->specs;
        for (auto& s : specs) {
            auto it = spec_values_.find(s.key());
            if (it != spec_values_.end()) s.definition = it->second;
        }
        return specs;
    }

    std::string specs_file()
    {
        if (spec_values_.empty() || project_->specs_path.empty()) return project_->specs_path;
        const std::string path = tdc::path_join(tdc::path_join(work_, "specs"), tdc::base_name(project_->specs_path));
        tdc::make_dirs(tdc::parent_dir(path));
        tdc::write_specs(project_->specs_path, path, spec_values_);
        return path;
    }

    // The 6571 the host detected; loads the project's sheets into it on first use.
    const std::string& instrument()
    {
        if (digital_.empty())
            throw PluginError(DM_E_STATE, "没有检测到 PXIe-6571：无法运行图形（设备列表里应有一块 PXIe-6571）");
        if (!instrument_ready_) {
            Json specs = Json::array(), levels = Json::array(), timing = Json::array();
            const std::string sp = specs_file();
            if (!sp.empty()) specs.push_back(sp);
            if (!project_->levels_path.empty()) levels.push_back(project_->levels_path);
            for (auto& t : project_->timing_files()) timing.push_back(t);
            host_call(digital_, "pattern_project",
                      Json{{"pinmap", project_->pinmap_path}, {"specs", specs}, {"levels", levels}, {"timing", timing}});
            instrument_ready_ = true;
        }
        return digital_;
    }

    std::string run_file(tdc::Pattern& p, bool single)
    {
        std::string data = p.patched_bytes();
        if (single) {
            const Json& a = analysis(p.file);
            if (a["outer_set_loop"].is_number()) {
                const size_t i = size_t(a["outer_set_loop"].as_int());
                auto saved = p.edits;
                p.edits[i] = 1;
                data = p.patched_bytes();
                p.edits = saved;
            }
        }
        if (!single && p.edits.empty()) return p.path;
        const std::string folder = tdc::path_join(work_, "run");
        tdc::make_dirs(folder);
        const std::string path = tdc::path_join(folder, p.file + (single ? "_once" : "") + ".digipat");
        tdc::write_file(path, data);
        return path;
    }

    Json gen_info()
    {
        tdc::Pattern& p = pattern();
        Json g = p.meta["summary"].is_object() ? p.meta["summary"] : Json::object();
        std::string note;
        for (auto& w : g["warnings"].items()) note += (note.empty() ? "" : "；") + w.as_string();
        g["vectors"] = int(p.base_ins.size());
        g["note"] = note.empty() ? "无（频率、延时都是时钟周期的整数倍）" : note;
        return g;
    }

    // ------------------------------------------------------------------ methods
    void register_methods()
    {
        method("info", "Project, instrument and mode", {}, [this](const Json&) {
            auto& pr = *project_;
            return Json{{"project", pr.folder}, {"name", tdc::base_name(pr.folder)},
                        {"pinmap", tdc::base_name(pr.pinmap_path)},
                        {"instrument", digital_.empty() ? "未检测到 PXIe-6571" : digital_},
                        {"resource", digital_.empty() ? "—" : digital_},
                        {"mode", digital_.empty() ? "未检测到 PXIe-6571" : simulate_ ? "仿真" : "NI-Digital"},
                        {"patterns", int(pr.patterns.size())}, {"current", current_},
                        {"levels", tdc::base_name(pr.levels_path)}, {"timing", tdc::base_name(pr.timing_path)},
                        {"specs", tdc::base_name(pr.specs_path)}, {"work_dir", work_}, {"busy_delay_us", busy_delay_ * 1e6}};
        });

        method("list_patterns", "Pattern names for the selector", {}, [this](const Json&) {
            project_->rescan();
            Json list = Json::array();
            for (auto& p : project_->patterns)
                list.push_back(Json{{"label", p->file + (p->modified() ? " *" : "")}, {"value", p->file}});
            return kv("patterns", list);
        });

        method("select", "Select the pattern the other methods work on", {"name"}, [this](const Json& a) {
            pattern(a.str("name"));
            current_ = a.str("name");
            return summary();
        });

        method("rescan", "Re-read the project folder (new or changed .digipat files)", {}, [this](const Json&) {
            project_->rescan();
            return kv("patterns", int(project_->patterns.size()));
        });

        method("summary", "TDC timing of the current pattern (START / HIT / BUSY, loops, run time)", {},
               [this](const Json&) { return summary(); });

        method("timeline", "Timeline of the first measurement, stacked digital traces (scope format)", {},
               [this](const Json&) { return analysis()["timeline"]; });

        method("vectors", "Decoded vectors of the current pattern", {}, [this](const Json&) {
            tdc::Pattern& p = pattern();
            const auto periods = project_->periods();
            Json rows = Json::array();
            std::string pins;
            for (auto& pin : p.pins) pins += (pins.empty() ? "" : " ") + pin;
            for (size_t i = 0; i < p.base_ins.size(); ++i) {
                const std::string ts = p.timeset_of(i);
                auto it = periods.find(ts);
                rows.push_back(Json{{"v", int(i)}, {"label", p.label_at(i)}, {"states", p.states[i]},
                                    {"instruction", p.instruction_text(i)}, {"timeset", ts},
                                    {"period", tdc::fmt_time(it != periods.end() ? it->second : NaN)},
                                    {"comment", p.comments[i]}, {"edited", p.edits.count(i) ? "✎" : ""}});
            }
            return Json{{"pattern", p.file}, {"label", p.name}, {"pins", pins}, {"rows", rows}, {"vector_count", int(rows.size())}};
        });

        method("counts", "Editable repeat / set_loop counts of the current pattern (params widget rows)", {}, [this](const Json&) {
            tdc::Pattern& p = pattern();
            const auto periods = project_->periods();
            const std::vector<uint32_t> ins = p.ins();
            Json rows = Json::array();
            for (size_t i = 0; i < ins.size(); ++i) {
                const tdc::Decoded d = tdc::decode(ins[i]);
                if (d.op != 1 && d.op != 4) continue;
                auto it = periods.find(p.timeset_of(i));
                const double period = it != periods.end() ? it->second : 0;
                std::string unit, note;
                if (d.op == 1) {
                    unit = "× " + tdc::fmt_time(period);
                    note = "= " + tdc::fmt_time(d.operand * period);
                } else {
                    const int end = p.loop_end(i);
                    unit = "次";
                    if (end >= 0) {
                        const unsigned li = tdc::decode(ins[size_t(end)]).operand & 0x7FFF;
                        char buf[16];
                        std::snprintf(buf, sizeof buf, "%02d", end);
                        note = "循环 " + (li < p.label_names.size() ? p.label_names[li] : std::string("?")) + "…v" + buf;
                    } else {
                        note = "循环（没有找到 end_loop）";
                    }
                }
                if (!p.comments[i].empty()) note += "  · " + p.comments[i];
                if (p.edits.count(i)) note += "  · 已修改";
                char name[32];
                std::snprintf(name, sizeof name, "v%02d %s", int(i), tdc::op_name(d.op).c_str());
                const std::string section = p.section_of(i);
                rows.push_back(Json{{"name", name}, {"group", section.empty() ? p.name : section},
                                    {"value", int(d.operand)}, {"unit", unit}, {"min", 1}, {"max", tdc::COUNT_MAX},
                                    {"type", "int"}, {"readonly", false}, {"note", note}});
            }
            return Json{{"rows", rows}, {"modified", p.modified()}};
        });

        method("set_count", "Change a repeat / set_loop count (name 'v07 repeat' or vector index)", {"name", "value"},
               [this](const Json& a) {
            const std::string name = a["name"].is_string() ? a.str("name") : std::to_string(a.integer("name", -1));
            const std::string digits = name.substr(name[0] == 'v' ? 1 : 0, name.find(' ') == std::string::npos ? std::string::npos
                                                                         : name.find(' ') - (name[0] == 'v' ? 1 : 0));
            char* end = nullptr;
            const long i = std::strtol(digits.c_str(), &end, 10);
            if (digits.empty() || (end && *end)) throw PluginError(DM_E_BAD_ARGS, "无效的向量 " + name);
            try {
                auto [applied, clamped] = pattern().set_count(size_t(i), std::llround(a.number("value", 0)));
                return Json{{"name", name}, {"value", applied}, {"clamped", clamped}};
            } catch (const std::invalid_argument& e) {
                throw PluginError(DM_E_BAD_ARGS, e.what());
            }
        });

        method("revert", "Discard the count edits of the current pattern", {}, [this](const Json&) {
            pattern().edits.clear();
            return summary();
        });

        method("save_as", "Save the edited pattern as <name>.digipat next to the original and select it", {"name"},
               [this](const Json& a) {
            tdc::Pattern& p = pattern();
            std::string name = a.str("name");
            if (name.empty()) {
                const auto t = std::time(nullptr);
                char buf[32];
                std::strftime(buf, sizeof buf, "_v%m%d_%H%M%S", std::localtime(&t));
                name = p.file + buf;
            }
            if (name.find_first_of("\\/:*?\"<>|") != std::string::npos)
                throw PluginError(DM_E_BAD_ARGS, "文件名不能包含 \\ / : * ? \" < > |");
            const std::string path = tdc::path_join(tdc::parent_dir(p.path), name + ".digipat");
            if (tdc::file_exists(path)) throw PluginError(DM_E_BAD_ARGS, name + ".digipat 已存在");
            tdc::write_file(path, p.patched_bytes());
            const std::string label = p.name;
            p.edits.clear();
            project_->rescan();
            current_ = name;
            log("info", "已保存 " + name + ".digipat（起始标签仍为 " + label + "）");
            return Json{{"saved", path}, {"current", name}};
        });

        // ---- burst ----
        method("burst", "Burst the current pattern on the detected 6571 (single=true: outer loop count 1); returns immediately",
               {"single"}, [this](const Json& a) {
            if (run_.state == "running") throw PluginError(DM_E_STATE, "图形正在运行");
            const bool single = a.boolean("single", false);
            tdc::Pattern& p = pattern();
            const Json& an = analysis();
            const std::string sheet = project_->timing_file.count(p.timesets.empty() ? "" : p.timesets[0])
                                          ? project_->timing_file[p.timesets[0]] : std::string();
            for (auto& ts : p.timesets)
                if (!project_->timing_file.count(ts) || project_->timing_file[ts] != sheet)
                    throw PluginError(DM_E_STATE, p.file + " 的时序集不在同一个已知的时序表里");
            // run time of this burst: the program with a jittered BUSY (a simulated 6571 waits this long)
            const tdc::RunResult rr = p.run(project_->periods(), busy_delay_, &rng_);
            const double est = single ? (rr.outer_iteration >= 0 ? rr.outer_iteration : rr.total) : rr.total;
            const std::string dev = instrument();
            const std::string path = run_file(p, single);
            try {
                if (!sheet.empty()) host_call(dev, "pattern_timing", kv("path", sheet));
                host_call(dev, "pattern_start", Json{{"path", path}, {"label", p.name}, {"sim_seconds", est}});
            } catch (const PluginError& e) {
                run_ = Run{};
                run_.state = "error";
                run_.pattern = p.file;
                run_.error = e.what();
                throw;
            }
            run_ = Run{};
            run_.state = "running";
            run_.pattern = p.file;
            run_.label = p.name;
            run_.single = single;
            run_.t0 = Clock::now();
            run_.est = est;
            run_.iteration = an["iteration"].as_number();
            run_.loops = single ? 1 : an["loops"].as_int(1);
            run_.pins = p.pins;
            log("info", "burst " + p.file + "（标签 " + p.name + (single ? "，单次" : "") + "，" + dev + "），预计 " + tdc::fmt_time(est));
            return status();
        });

        method("abort", "Abort a running burst", {}, [this](const Json&) {
            if (run_.state != "running") return status();
            host_call(digital_, "pattern_abort");
            run_.state = "aborted";
            run_.t_end = Clock::now();
            run_.ended = true;
            log("warning", "已中止 " + run_.pattern);
            return status();
        });

        method("status", "Burst state, progress and result", {}, [this](const Json&) { return status(); });

        // ---- project sheets ----
        method("specs", "Specifications as editable rows (params widget)", {}, [this](const Json&) {
            const auto specs = specs_now();
            const auto sym = tdc::spec_symbols(specs);
            Json rows = Json::array();
            for (auto& s : specs) {
                double value = 0;
                std::string unit;
                const bool plain = tdc::split_quantity(s.definition, value, unit);
                auto it = sym.find(s.key());
                rows.push_back(Json{{"name", s.key()}, {"group", s.section},
                                    {"value", plain ? value : (it != sym.end() ? it->second : 0.0)}, {"unit", unit},
                                    {"min", 0}, {"max", 0}, {"type", "float"}, {"readonly", !plain},
                                    {"note", s.comment + (spec_values_.count(s.key()) ? "  · 已修改" : "")}});
            }
            return kv("rows", rows);
        });

        method("set_spec", "Change a specification value (keeps its unit), re-apply levels and timing", {"name", "value"},
               [this](const Json& a) {
            const std::string name = a.str("name");
            const tdc::Spec* spec = nullptr;
            for (auto& s : project_->specs) if (s.key() == name) spec = &s;
            if (!spec) throw PluginError(DM_E_BAD_ARGS, "没有规格 " + name);
            double old = 0;
            std::string unit;
            tdc::split_quantity(spec_values_.count(name) ? spec_values_[name] : spec->definition, old, unit);
            const double value = a.number("value", 0);
            const std::string text = tdc::fmt_g(value, 9) + (unit.empty() ? "" : " " + unit);
            double orig = 0;
            std::string ounit;
            if (tdc::split_quantity(spec->definition, orig, ounit) && orig == value && ounit == unit) spec_values_.erase(name);
            else spec_values_[name] = text;
            if (instrument_ready_) host_call(digital_, "pattern_specs", kv("path", specs_file()));
            log("info", "规格 " + name + " = " + text);
            return Json{{"name", name}, {"value", value}, {"clamped", false}};
        });

        method("levels", "Pin levels with formulas evaluated against the specifications", {}, [this](const Json&) {
            const auto sym = tdc::spec_symbols(specs_now());
            Json rows = Json::array();
            for (auto& lv : project_->levels) {
                Json row{{"pin", lv.pin}};
                auto f = [&lv](const char* k) { auto it = lv.fields.find(k); return it != lv.fields.end() ? it->second : std::string(); };
                row["mode"] = f("TerminationMode");
                std::string formulas;
                for (const char* k : {"Vil", "Vih", "Vol", "Voh", "Iol", "Ioh", "Vterm"}) {
                    const std::string text = f(k);
                    std::string key = k;
                    for (auto& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
                    try {
                        const double v = tdc::evaluate(text, sym);
                        row[key] = k[0] == 'I' ? tdc::fmt_g(v * 1e3, 6) + " mA" : tdc::fmt_g(v, 6) + " V";
                        for (auto& [s, _] : sym)
                            if (text.find(s) != std::string::npos) { formulas += (formulas.empty() ? "" : "，") + std::string(k) + " = " + text; break; }
                    } catch (const std::exception&) {
                        row[key] = text;
                    }
                }
                row["formulas"] = formulas.empty() ? "—" : formulas;
                rows.push_back(row);
            }
            return Json{{"rows", rows}, {"file", tdc::base_name(project_->levels_path)}};
        });

        method("timesets", "Timesets of the timing sheets", {}, [this](const Json&) {
            Json rows = Json::array();
            for (auto& [name, ts] : project_->timing) {
                std::vector<std::string> used;
                for (auto& p : project_->patterns)
                    for (auto& t : p->timesets) if (t == name) { used.push_back(p->file); break; }
                std::string list;
                for (size_t i = 0; i < used.size() && i < 2; ++i) list += (i ? ", " : "") + used[i];
                if (used.size() > 2) list += " 等 " + std::to_string(used.size()) + " 个";
                rows.push_back(Json{{"name", name}, {"period", ts.period_text}, {"used_by", int(used.size())}, {"patterns", list}});
            }
            return kv("rows", rows);
        });

        method("pins", "Pin map: pins, channels, groups", {}, [this](const Json&) {
            const auto& pm = project_->pinmap;
            Json rows = Json::array();
            for (auto& pin : pm.pins) {
                std::string groups;
                for (auto& [g, ps] : pm.groups)
                    for (auto& x : ps) if (x == pin) { groups += (groups.empty() ? "" : ", ") + g; break; }
                int used = 0;
                for (auto& p : project_->patterns)
                    for (auto& x : p->pins) if (x == pin) { ++used; break; }
                auto c = pm.connections.find(pin);
                rows.push_back(Json{{"pin", pin}, {"instrument", c != pm.connections.end() ? Json(c->second.instrument) : Json("—")},
                                    {"channel", c != pm.connections.end() ? Json(c->second.channel) : Json("—")},
                                    {"groups", groups}, {"patterns", used}});
            }
            return kv("rows", rows);
        });

        method("sim_set_busy", "[sim] TDC conversion time before BUSY rises again", {"us"}, [this](const Json& a) {
            busy_delay_ = std::max(0.0, a.number("us", 120)) * 1e-6;
            return kv("busy_delay_us", busy_delay_ * 1e6);
        });

        // ---- generator ----
        method("pin_options", "Pins of the pin map for the generator's selectors", {"optional"}, [this](const Json& a) {
            Json list = Json::array();
            if (a.boolean("optional", false)) list.push_back(Json{{"label", "（不用）"}, {"value", ""}});
            for (auto& p : project_->pinmap.pins) list.push_back(Json{{"label", p}, {"value", p}});
            return kv("pins", list);
        });

        method("gen_params", "Generator parameters: the last generated set, else the defaults", {}, [this](const Json&) {
            Json p = tdc::gen_defaults();
            for (auto& [k, v] : gen_last_["params"].members()) p[k] = v;
            return p;
        });

        method("generate", "Generate a CLK / TRIG / OUT1 / OUT2 stimulus pattern, select it and return its timing",
               {"name", "clk_pin", "trig_pin", "out1_pin", "out2_pin", "clock_mhz", "edge", "out1_khz", "out1_cycles",
                "out1_delay_us", "out2_khz", "out2_cycles", "out2_delay_us", "post_us", "loops"},
               [this](const Json& a) {
            if (run_.state == "running") throw PluginError(DM_E_STATE, "图形正在运行，先中止");
            tdc::GenPlan pl;
            try {
                pl = tdc::gen_plan(a, project_->pinmap.pins);
            } catch (const tdc::GenError& e) {
                throw PluginError(DM_E_BAD_ARGS, e.what());
            }
            if (auto clash = project_->find(pl.name); clash && clash->path.rfind(gen_dir_, 0) != 0)
                throw PluginError(DM_E_BAD_ARGS, "工程里已有图形 " + pl.name + "，请换个名字");
            std::string tpl = gen_template_;
            if (!tdc::file_exists(tpl))
                for (auto& p : project_->patterns) if (!p->generated) { tpl = p->path; break; }
            const std::string path = tdc::path_join(gen_dir_, pl.name + ".digipat");
            try {
                tdc::write_file(path, tdc::gen_build(pl, tdc::read_file(tpl)));
            } catch (const tdc::GenError& e) {
                throw PluginError(DM_E_BAD_ARGS, e.what());
            }
            tdc::gen_timing_sheet(pl, project_->pinmap.pins, gen_dir_);
            const Json summary = tdc::gen_summary(pl);
            Json roles{{"CLK", pl.clk}, {"TRIG", pl.trig}, {"OUT1", pl.out1}, {"OUT2", pl.out2}};
            gen_last_ = Json{{"params", pl.params}, {"roles", roles}, {"summary", summary}};
            tdc::write_file(tdc::path_join(gen_dir_, pl.name + ".tdcgen.json"), gen_last_.dump());
            project_->rescan();
            current_ = pl.name;
            const Json& an = analysis();
            const int vectors = int(pattern().base_ins.size());
            log("info", "已生成 " + pl.name + "：" + std::to_string(vectors) + " 个向量，单次测量 " + tdc::fmt_time(an["iteration"].as_number()));
            Json out = summary;
            out["name"] = pl.name;
            out["vectors"] = vectors;
            out["out1_start_text"] = tdc::fmt_time(pl.w1.start);
            out["out2_start_text"] = tdc::fmt_time(pl.w2.start);
            out["delay_text"] = tdc::fmt_time(pl.w2.start - pl.w1.start);
            out["measure_text"] = tdc::fmt_time(an["iteration"].as_number());
            out["total_text"] = tdc::fmt_time(an["total"].as_number());
            out["note"] = tdc::gen_note(pl);
            out["period_text"] = tdc::fmt_time(pl.period);
            return out;
        });

        method("save_generated", "Copy the current generated pattern (and its timing sheet) into the project folder", {},
               [this](const Json&) {
            tdc::Pattern& p = pattern();
            if (!p.generated || p.path.rfind(gen_dir_, 0) != 0) throw PluginError(DM_E_STATE, "当前图形不是刚生成的图形");
            const std::string dest = tdc::path_join(project_->folder, "Patterns");
            const std::string tdest = tdc::path_join(project_->folder, "Timing");
            tdc::make_dirs(tdest);
            const std::string target = tdc::path_join(dest, p.file + ".digipat");
            if (tdc::file_exists(target)) throw PluginError(DM_E_BAD_ARGS, "工程里已有 " + p.file + ".digipat");
            const std::string sheet = project_->timing_file[p.timesets.at(0)];
            const std::string tsheet = tdc::path_join(tdest, tdc::base_name(sheet));
            tdc::write_file(tsheet, tdc::read_file(sheet));
            const std::string side = tdc::path_join(gen_dir_, p.file + ".tdcgen.json");
            tdc::write_file(target, tdc::read_file(p.path));
            tdc::write_file(tdc::path_join(dest, p.file + ".tdcgen.json"), tdc::read_file(side));
            std::remove(p.path.c_str());
            std::remove(side.c_str());
            project_->rescan();
            log("info", "已保存 " + p.file + ".digipat 和时序表 " + tdc::base_name(sheet) + " 到工程");
            return Json{{"saved", target}, {"timing", tsheet}};
        });
    }

    Json summary()
    {
        const Json& a = analysis();
        Json out = Json::object();
        for (auto& [k, v] : a.members())
            if (k != "timeline" && k != "events") out[k] = v;
        auto f = [](const Json& v) { return tdc::fmt_time(v.is_number() ? v.as_number() : NaN); };
        std::string roles;
        for (auto& [pin, role] : a["roles"].members()) roles += (roles.empty() ? "" : "  ") + pin + "=" + role.as_string();
        std::string notes;
        for (auto& n : a["notes"].items()) notes += (notes.empty() ? "" : "；") + n.as_string();
        out["total_text"] = f(a["total"]);
        out["iteration_text"] = f(a["iteration"]);
        out["loops_text"] = "× " + std::to_string(a["loops"].as_int(1));
        out["roles_text"] = roles.empty() ? "—" : roles;
        out["notes_text"] = notes.empty() ? "无" : notes;
        out["gen"] = a["generated"].as_bool() ? gen_info() : Json();
        out["current"] = current_;
        Json rows = Json::array();
        auto row = [&rows](const char* item, Json value) { rows.push_back(Json{{"item", item}, {"value", std::move(value)}}); };
        row("START 引脚", a["start_pin"]);
        row("HIT 引脚", a["hit_pin"]);
        row("BUSY 引脚", a["busy_pin"]);
        row("START 宽度", f(a["start_width"]));
        row("START → HIT1", f(a["start_to_hit"]));
        row("HIT 个数", a["hit_count"]);
        row("HIT 周期", f(a["hit_period"]));
        row("HIT 宽度", f(a["hit_width"]));
        row("BUSY 等待（仿真）", f(a["busy_wait"]));
        row("单次测量", f(a["iteration"]));
        row("循环次数", a["loops"]);
        row("总时间", out["total_text"]);
        out["rows"] = rows;
        Json ev = Json::array();
        size_t n = 0;
        for (auto& e : a["events"].items()) {
            if (++n > 200) break;
            ev.push_back(Json{{"event", e["event"]}, {"pin", e["pin"].is_string() ? e["pin"] : Json("—")}, {"t", f(e["t"])},
                              {"rel", e["rel"].is_number() ? Json(f(e["rel"])) : Json("—")},
                              {"width", e["width"].is_number() ? Json(f(e["width"])) : Json("—")}});
        }
        out["events_rows"] = ev;
        return out;
    }

    Json status()
    {
        Run& st = run_;
        if (st.state == "running") {
            const double elapsed = secs(Clock::now() - st.t0);
            try {
                const Json r = host_call(digital_, "pattern_status", kv("pins", [&] { Json p = Json::array(); for (auto& x : st.pins) p.push_back(x); return p; }()));
                if (r.boolean("done", false)) {
                    st.state = "done";
                    // a simulated burst ends when the program would, not at the next poll
                    st.t_end = simulate_ ? st.t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(st.est))
                                         : Clock::now();
                    st.ended = true;
                    st.pass = r["pass"];
                    st.fails = r["fail_count"];
                    const bool failed = st.pass.is_bool() && !st.pass.as_bool();
                    log(failed ? "error" : "info", st.pattern + " 完成：" + (st.pass.is_bool() ? (st.pass.as_bool() ? "通过" : "失败") : "结束") +
                                                       "，用时 " + tdc::fmt_time(secs(st.t_end - st.t0), 4));
                }
            } catch (const PluginError& e) {
                st.state = "error";
                st.error = e.what();
                st.t_end = Clock::now();
                st.ended = true;
                (void)elapsed;
            }
        }
        Json out{{"state", st.state}, {"state_text", state_text(st.state)},
                 {"pattern", st.pattern.empty() ? current_ : st.pattern}, {"running", st.state == "running"},
                 {"mode", digital_.empty() ? "未检测到 PXIe-6571" : simulate_ ? "仿真" : "NI-Digital"}};
        if (st.state != "idle" && !st.pattern.empty() && st.t0 != Clock::time_point{}) {
            const double elapsed = secs((st.ended ? st.t_end : Clock::now()) - st.t0);
            const double est = std::max(st.est, 1e-9);
            const int done = st.state == "done" ? st.loops
                             : st.iteration > 0 ? std::min(st.loops, int(elapsed / st.iteration)) : 0;
            out["elapsed"] = elapsed;
            out["elapsed_text"] = tdc::fmt_time(elapsed);
            out["est_text"] = tdc::fmt_time(st.est);
            out["progress"] = st.state == "done" ? 100.0 : std::min(99.9, 100.0 * elapsed / est);
            out["loops_done"] = done;
            out["loops"] = st.loops;
            out["time_text"] = tdc::fmt_time(elapsed, 3) + " / " + tdc::fmt_time(st.est, 3);
            out["loops_text"] = std::to_string(done) + " / " + std::to_string(st.loops);
        } else {
            out["time_text"] = "—";
            out["loops_text"] = "—";
        }
        if (st.state == "done" || st.state == "aborted" || st.state == "error") {
            out["pass"] = st.pass;
            out["result"] = st.pass.is_bool() ? (st.pass.as_bool() ? "PASS" : "FAIL")
                            : st.state == "aborted" ? "ABORT" : st.state == "error" ? "ERROR" : "—";
            std::string fails;
            for (auto& [pin, n] : st.fails.members())
                if (n.as_int() != 0) fails += (fails.empty() ? "" : "，") + pin + ": " + std::to_string(n.as_int());
            out["fail_text"] = fails.empty() ? "无" : fails;
            if (!st.error.empty()) out["error"] = st.error;
        } else {
            out["result"] = "—";
        }
        return out;
    }

    struct Run {
        std::string state = "idle", pattern, label, error;
        bool single = false, ended = false;
        Clock::time_point t0{}, t_end{};
        double est = 0, iteration = 0;
        int loops = 1;
        Json pass, fails = Json::object();
        std::vector<std::string> pins;
    };

    std::string plugin_dir_, work_, gen_dir_, gen_template_, current_, digital_;
    std::unique_ptr<tdc::Project> project_;
    double busy_delay_ = 120e-6;
    bool simulate_ = true, instrument_ready_ = false;
    std::map<std::string, std::string> spec_values_;
    std::map<std::string, Json> cache_;
    Json gen_last_ = Json::object();
    Run run_;
    std::mt19937 rng_;
};

DM_EXPORT_PLUGIN(TdcMeasure, kManifest)
