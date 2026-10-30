// tdc_task.cpp — TDC测量: a test task (one DLL) that runs TDC stimulus patterns on a PXIe-6571.
//
// The task owns the Digital Pattern Editor project (DemoPrj-TDC), decodes and analyses its
// patterns, edits repeat / set_loop counts, generates four-pin stimulus patterns (CLK / TRIG /
// OUT1 / OUT2), and bursts them on the 6571 the host detected. It never opens the instrument
// itself: every instrument action is a host call to that device ("pattern_project",
// "pattern_timing", "pattern_specs", "pattern_start", "pattern_status", "pattern_abort").
#include "project.hpp"
#include "sheets.hpp"
#include "tdcgen.hpp"
#include "util.hpp"
#include "workspace.hpp"

#include <dm/plugin.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <random>

using dm::Json;
using dm::PluginError;
using Clock = std::chrono::steady_clock;

static const char* kManifest = R"({
  "id": "tdc_measure",
  "name": "TDC测量",
  "version": "0.5.0",
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
        if (!is_absolute(folder)) folder = tdc::path_join(plugin_dir_, folder);
        work_ = o.str("work_dir", tdc::path_join(plugin_dir_, "_work"));
        gen_dir_ = tdc::path_join(work_, "gen");
        tdc::make_dirs(gen_dir_);
        projects_dir_ = o.str("projects_dir", tdc::path_join(work_, "projects"));
        instrument_name_ = o.str("instrument_name", cfg.str("resource", ""));
        seed_pattern_ = tdc::path_join(plugin_dir_, "project/DemoPrj-TDC/Patterns/TDC_M20_2.digipat");
        export_root_ = o.str("export_root", tdc::parent_dir(plugin_dir_));
        busy_delay_ = o.number("busy_delay_us", 120.0) * 1e-6;
        digital_ = cfg["instruments"]["digital"].as_string();
        simulate_ = cfg.boolean("simulate", true);
        if (o.boolean("create_project", false) && tdc::find_files(folder, ".pinmap").empty())
            tdc::new_project(folder, seed_pattern_, instrument_name_);
        open_project(folder, o.str("pattern", "TDC_M20_2"));
        rng_.seed(std::random_device{}());
        register_methods();
    }

private:
    static bool is_absolute(const std::string& path)
    {
        return path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\');
    }

    void open_project(const std::string& folder, const std::string& want)
    {
        auto next = std::make_unique<tdc::Project>(folder, std::vector<std::string>{gen_dir_});
        if (next->patterns.empty()) throw std::runtime_error(folder + "：没有 .digipat 图形");
        project_ = std::move(next);
        current_ = project_->find(want) ? want : project_->patterns.front()->file;
        gen_template_ = tdc::path_join(project_->folder, "Patterns/TDC_M20_2.digipat");
        spec_values_.clear();
        instrument_ready_ = false;
        ++sheet_rev_;
        run_ = Run{};
    }

    // ------------------------------------------------------------------ helpers
    tdc::Pattern& pattern(const std::string& name = {})
    {
        auto p = project_->find(name.empty() ? current_ : name);
        if (!p) throw PluginError(DM_E_BAD_ARGS, "没有图形 " + (name.empty() ? current_ : name));
        return *p;
    }

    // Timesets evaluated against the specifications as edited on the page.
    tdc::TimingModel model() const { return project_->model(tdc::spec_symbols(specs_now())); }

    static double period_of(const tdc::TimingModel& tm, const std::string& ts)
    {
        auto it = tm.find(ts);
        return it != tm.end() && it->second.period > 0 ? it->second.period : NaN;
    }

    const Json& analysis(const std::string& name = {})
    {
        tdc::Pattern& p = pattern(name);
        std::string key = p.file + "|" + std::to_string(p.mtime) + "|" + std::to_string(busy_delay_) + "|" +
                          std::to_string(p.revision) + "|" + std::to_string(sheet_rev_);
        for (auto& [i, v] : p.edits) key += "|" + std::to_string(i) + "=" + std::to_string(v);
        for (auto& [k, v] : spec_values_) key += "|" + k + "=" + v;
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            if (cache_.size() > 64) cache_.clear();
            const tdc::TimingModel tm = model();
            Json a = tdc::analyze(p, tm, busy_delay_);
            Json checks = Json::array();
            for (auto& c : tdc::check(p, *project_, tm)) {
                checks.push_back(c);
                a["notes"].push_back(c);
            }
            a["checks"] = checks;
            it = cache_.emplace(key, std::move(a)).first;
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
        if (!single && !p.modified()) return p.path;
        const std::string folder = tdc::path_join(work_, "run");
        tdc::make_dirs(folder);
        const std::string path = tdc::path_join(folder, p.file + (single ? "_once" : "") + ".digipat");
        tdc::write_file(path, data);
        return path;
    }

    // ------------------------------------------------------------------ editing
    // Runs an edit while no burst is active; invalid input becomes a page error.
    Json editing(const std::function<Json()>& edit)
    {
        if (run_.state == "running") throw PluginError(DM_E_STATE, "图形正在运行，先中止");
        try {
            return edit();
        } catch (const std::invalid_argument& e) {
            throw PluginError(DM_E_BAD_ARGS, e.what());
        }
    }

    std::string backup_root() const { return tdc::path_join(tdc::path_join(work_, "backup"), tdc::base_name(project_->folder)); }

    // First edit of a project file keeps the original under _work/backup for restore_sheets.
    void backup(const std::string& path)
    {
        const std::string& root = project_->folder;
        if (path.rfind(root, 0) != 0 || path.size() <= root.size()) return;
        const std::string dst = tdc::path_join(backup_root(), path.substr(root.size() + 1));
        if (tdc::file_exists(dst)) return;
        tdc::make_dirs(tdc::parent_dir(dst));
        tdc::write_file(dst, tdc::read_file(path));
    }

    // Edit a sheet on disk; the next burst loads it into the instrument again.
    void edit_sheet(const std::string& path, const std::function<void(std::string&)>& edit)
    {
        if (path.empty()) throw std::invalid_argument("工程里没有这个文件");
        std::string text = tdc::read_file(path);
        edit(text);
        backup(path);
        tdc::write_file(path, text);
        project_->reload_sheets();
        instrument_ready_ = false;
        ++sheet_rev_;
    }

    std::string sheet_of(const std::string& timeset) const
    {
        auto it = project_->timing_file.find(timeset);
        if (it == project_->timing_file.end()) throw std::invalid_argument("没有时序集 " + timeset);
        return it->second;
    }

    // A period, edge or level: a quantity or a formula over the specifications.
    std::string checked(const std::string& raw, const std::string& what, bool positive = false) const
    {
        const std::string text = tdc::trim(raw);
        double v = 0;
        try {
            if (text.empty() || !tdc::parse_quantity(text, v)) v = tdc::evaluate(text, tdc::spec_symbols(specs_now()));
        } catch (const std::exception&) {
            throw std::invalid_argument(what + " '" + text + "' 无法求值");
        }
        if (positive && !(v > 0)) throw std::invalid_argument(what + " 必须大于 0");
        return text;
    }

    // Timesets a pattern can use: one burst applies one timing sheet.
    std::vector<std::string> sheet_timesets(const tdc::Pattern& p) const
    {
        auto first = project_->timing_file.find(p.timesets.empty() ? std::string() : p.timesets[0]);
        std::vector<std::string> out;
        for (auto& [name, ts] : project_->timing) {
            (void)ts;
            if (first == project_->timing_file.end() || project_->timing_file.at(name) == first->second) out.push_back(name);
        }
        return out;
    }

    // A change of the vector table: undoable as one step, and all or none when it fails.
    Json pattern_edit(const std::function<Json(tdc::Pattern&)>& edit)
    {
        return editing([&] {
            tdc::Pattern& p = pattern();
            p.checkpoint();
            try {
                return edit(p);
            } catch (...) {
                p.undo();
                throw;
            }
        });
    }

    void edit_cell(tdc::Pattern& p, const Json& a)
    {
        const int v = a.integer("v", -1);
        if (v < 0) throw std::invalid_argument("无效的向量");
        const std::string field = a.str("field"), value = a.str("value");
        if (field.rfind("pin:", 0) == 0) {
            p.set_state(size_t(v), field.substr(4), tdc::trim(value));
        } else if (field == "instruction") {
            p.set_instruction(size_t(v), value);
        } else if (field == "label") {
            p.set_label(size_t(v), value);
        } else if (field == "comment") {
            p.set_comment(size_t(v), value);
        } else if (field == "timeset") {
            const auto allowed = sheet_timesets(p);
            if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
                throw std::invalid_argument("时序集 " + value + " 不在这个图形的时序表里");
            p.set_timeset(size_t(v), value);
        } else {
            throw std::invalid_argument("未知字段 " + field);
        }
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

        // ---- projects and export ----
        method("project_list", "Projects in the projects folder, for the selector", {}, [this](const Json&) {
            Json list = Json::array();
            for (auto& name : tdc::sub_dirs(projects_dir_))
                list.push_back(Json{{"label", name}, {"value", tdc::path_join(projects_dir_, name)}});
            return kv("projects", list);
        });

        method("project_open", "Open a project: a folder in the projects folder, or a full path", {"path"}, [this](const Json& a) {
            return editing([&] {
                const std::string raw = tdc::trim(a.str("path"));
                if (raw.empty()) throw std::invalid_argument("请填写工程名或完整路径");
                const std::string folder = is_absolute(raw) ? raw : tdc::path_join(projects_dir_, raw);
                if (tdc::find_files(folder, ".pinmap").empty()) throw std::invalid_argument(folder + " 里没有 .pinmap，不是工程");
                open_project(folder, std::string());
                log("info", "已打开工程 " + project_->folder);
                return kv("project", project_->folder);
            });
        });

        method("project_new", "Create a project in the projects folder and open it", {"name"}, [this](const Json& a) {
            return editing([&] {
                const std::string name = tdc::trim(a.str("name"));
                if (!tdc::valid_name(name)) throw std::invalid_argument("工程名 '" + name + "' 无效：只能用字母、数字和下划线，且不能以数字开头");
                const std::string folder = tdc::path_join(projects_dir_, name);
                if (tdc::file_exists(folder)) throw std::invalid_argument("已有工程 " + name);
                tdc::new_project(folder, seed_pattern_, instrument_name_);
                open_project(folder, "main");
                log("info", "已新建工程 " + name);
                return kv("project", folder);
            });
        });

        method("pattern_new", "Add a pattern to the project: one halt vector on the connected pins", {"name"}, [this](const Json& a) {
            return editing([&] {
                const std::string name = tdc::trim(a.str("name"));
                if (!tdc::valid_name(name)) throw std::invalid_argument("图形名 '" + name + "' 无效：只能用字母、数字和下划线，且不能以数字开头");
                if (project_->find(name)) throw std::invalid_argument("已有图形 " + name);
                if (project_->timing.empty()) throw std::invalid_argument("工程里没有时序集");
                std::vector<std::string> pins;
                for (auto& pin : project_->pinmap.pins) if (project_->pinmap.connections.count(pin)) pins.push_back(pin);
                const std::string path = tdc::path_join(tdc::path_join(project_->folder, "Patterns"), name + ".digipat");
                tdc::make_dirs(tdc::parent_dir(path));
                tdc::write_file(path, pattern().data);
                project_->rescan();
                tdc::Pattern& p = pattern(name);
                try {
                    p.clear(name, pins, project_->timing.front().first);
                } catch (const std::exception&) {
                    std::remove(path.c_str());
                    project_->rescan();
                    throw;
                }
                const std::string bytes = p.patched_bytes();
                tdc::write_file(path, bytes);
                p.commit(bytes);
                p.mtime = tdc::file_mtime(path);
                current_ = name;
                return kv("pattern", name);
            });
        });

        method("export_plugin", "Export the project as a task plugin: a folder with the runner DLL, plugin.json, page and project",
               {"id", "name"}, [this](const Json& a) {
            return editing([&] {
                for (auto& p : project_->patterns)
                    if (p->modified()) throw std::invalid_argument("图形 " + p->file + " 有未保存的修改，先保存");
                tdc::ExportSpec s;
                s.id = tdc::trim(a.str("id"));
                s.name = tdc::trim(a.str("name"));
                if (s.name.empty()) s.name = s.id;
                s.version = "1.0.0";
                s.pattern = current_;
                s.project = project_->folder;
                s.page = tdc::path_join(plugin_dir_, "page.json");
                s.library = tdc::library_path();
                s.root = export_root_;
                if (tdc::path_join(s.root, s.id) == tdc::parent_dir(project_->folder))
                    throw std::invalid_argument("当前工程就在要导出的插件里，请换一个 id");
                const std::string dest = tdc::export_plugin(s);
                log("info", "已导出插件 " + s.id + "：" + dest + "，点“重新扫描插件”加载");
                return Json{{"id", s.id}, {"folder", dest}, {"text", dest}};
            });
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
            const tdc::TimingModel tm = model();
            Json rows = Json::array(), pin_list = Json::array(), ts_options = Json::array();
            std::string pins;
            for (auto& pin : p.pins) {
                pins += (pins.empty() ? "" : " ") + pin;
                pin_list.push_back(pin);
            }
            for (auto& ts : sheet_timesets(p)) ts_options.push_back(ts);
            for (size_t i = 0; i < p.base_ins.size(); ++i) {
                const std::string ts = p.timeset_of(i);
                rows.push_back(Json{{"v", int(i)}, {"label", p.label_at(i)}, {"states", p.states[i]},
                                    {"instruction", p.instruction_text(i)}, {"timeset", ts},
                                    {"period", tdc::fmt_time(period_of(tm, ts))},
                                    {"comment", p.comments[i]}, {"edited", p.edits.count(i) ? "✎" : ""}});
            }
            std::string rev = p.file + "|" + std::to_string(p.revision) + "|" + std::to_string(sheet_rev_) + "|" + std::to_string(p.mtime);
            for (auto& [i, v] : p.edits) rev += "|" + std::to_string(i) + "=" + std::to_string(v);
            for (auto& [k, v] : spec_values_) rev += "|" + k + "=" + v;
            return Json{{"pattern", p.file}, {"label", p.name}, {"pins", pins}, {"pin_list", pin_list},
                        {"ts_options", ts_options}, {"rows", rows}, {"vector_count", int(rows.size())}, {"rev", rev},
                        {"modified", p.modified()}};
        });

        method("edit_vector", "Change one cell of the vector table (field: pin:<name>, instruction, timeset, label, comment)",
               {"v", "field", "value"}, [this](const Json& a) {
            return pattern_edit([&](tdc::Pattern& p) {
                edit_cell(p, a);
                return Json{{"modified", p.modified()}};
            });
        });

        method("edit_vectors", "Change several cells at once, all or none: cells = [{v, field, value}, ...]", {"cells"}, [this](const Json& a) {
            return pattern_edit([&](tdc::Pattern& p) {
                for (auto& cell : a["cells"].items()) edit_cell(p, cell);
                return Json{{"cells", int(a["cells"].size())}, {"modified", p.modified()}};
            });
        });

        method("vector_insert", "Insert count vectors after v: same pin states and timeset, no instruction", {"v", "count"}, [this](const Json& a) {
            return pattern_edit([&](tdc::Pattern& p) {
                const int v = std::max(0, a.integer("v", 0));
                for (int i = 0; i < std::max(1, a.integer("count", 1)); ++i) p.insert_vector(size_t(v));
                return kv("vectors", int(p.base_ins.size()));
            });
        });

        method("vector_delete", "Delete count vectors from v; a label on them moves to the next vector", {"v", "count"}, [this](const Json& a) {
            return pattern_edit([&](tdc::Pattern& p) {
                const int v = std::max(0, a.integer("v", 0));
                for (int i = 0; i < std::max(1, a.integer("count", 1)); ++i) p.remove_vector(size_t(v));
                return kv("vectors", int(p.base_ins.size()));
            });
        });

        method("vector_undo", "Undo the last change of the vector table", {}, [this](const Json&) {
            return editing([&] {
                if (!pattern().undo()) throw std::invalid_argument("没有可撤销的修改");
                return kv("modified", pattern().modified());
            });
        });

        method("pattern_add_pin", "Add a pin of the pin map to the current pattern (a column of X)", {"pin"}, [this](const Json& a) {
            return editing([&] {
                const std::string pin = a.str("pin");
                const auto& pins = project_->pinmap.pins;
                if (std::find(pins.begin(), pins.end(), pin) == pins.end()) throw std::invalid_argument("引脚 " + pin + " 不在引脚映射里");
                tdc::Pattern& p = pattern();
                p.checkpoint();
                p.add_pin(pin);
                return kv("pin", pin);
            });
        });

        method("pattern_remove_pin", "Remove a pin column from the current pattern", {"pin"}, [this](const Json& a) {
            return editing([&] {
                tdc::Pattern& p = pattern();
                if (std::find(p.pins.begin(), p.pins.end(), a.str("pin")) != p.pins.end()) p.checkpoint();
                p.remove_pin(a.str("pin"));
                return kv("pin", a.str("pin"));
            });
        });

        method("pattern_save", "Write the edits to the current pattern's own .digipat", {}, [this](const Json&) {
            return editing([&] {
                tdc::Pattern& p = pattern();
                if (!p.modified()) return kv("saved", p.path);
                const std::string bytes = p.patched_bytes();
                backup(p.path);
                tdc::write_file(p.path, bytes);
                p.commit(bytes);
                p.mtime = tdc::file_mtime(p.path);
                log("info", "已保存 " + p.file + ".digipat");
                return kv("saved", p.path);
            });
        });

        method("counts", "Editable repeat / set_loop counts of the current pattern (params widget rows)", {}, [this](const Json&) {
            tdc::Pattern& p = pattern();
            const tdc::TimingModel tm = model();
            const std::vector<uint32_t> ins = p.ins();
            Json rows = Json::array();
            for (size_t i = 0; i < ins.size(); ++i) {
                const tdc::Decoded d = tdc::decode(ins[i]);
                if (d.op != 1 && d.op != 4) continue;
                const double pv = period_of(tm, p.timeset_of(i));
                const double period = std::isfinite(pv) ? pv : 0;
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

        method("revert", "Discard the unsaved edits of the current pattern", {}, [this](const Json&) {
            pattern().reset();
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
            std::string bytes;
            try {
                bytes = p.patched_bytes();
            } catch (const std::invalid_argument& e) {
                throw PluginError(DM_E_BAD_ARGS, e.what());
            }
            tdc::write_file(path, bytes);
            const std::string label = p.name;
            p.reset();
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
            const tdc::RunResult rr = p.run(model(), busy_delay_, &rng_);
            double est = single ? (rr.outer_iteration >= 0 ? rr.outer_iteration : rr.total) : rr.total;
            // Manual trigger (simulation): the program stops in every TRIG wait loop until the user
            // gives an edge. One measurement = the part of an iteration outside the wait.
            const bool manual = manual_trigger_ && simulate_ && !rr.waits.empty();
            double active = 0;
            if (manual) {
                const double iter = rr.outer_iteration >= 0 ? rr.outer_iteration : rr.total;
                double waited = 0;
                for (auto& w : rr.waits) waited += w.second;
                active = std::max(iter - waited, 1e-9);
                est = 1e9;                                   // the simulated 6571 runs until the task stops it
            }
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
            run_.manual = manual;
            run_.active = active;
            if (manual) {
                run_.est = active * run_.loops;
                log("info", "burst " + p.file + "（手动触发）：每次 TRIG 上升沿执行一次测量，共 " + std::to_string(run_.loops) + " 次");
                return status();
            }
            if (manual_trigger_ && simulate_) log("warning", p.file + " 没有等待 TRIG 的循环，按自动方式运行");
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

        method("set_trigger_mode", "[sim] manual=true: a burst waits in each TRIG wait loop until trigger() is called",
               {"manual"}, [this](const Json& a) {
            manual_trigger_ = a.boolean("manual", false);
            log("info", manual_trigger_ ? "仿真触发：手动（点“给 TRIG 上升沿”）" : "仿真触发：自动（芯片转换完 BUSY 自动拉高）");
            return kv("manual", manual_trigger_);
        });

        method("trigger_mode", "[sim] current trigger mode", {}, [this](const Json&) {
            return Json{{"manual", manual_trigger_}, {"text", manual_trigger_ ? "手动" : "自动"}};
        });

        method("trigger", "[sim] Give TRIG a low->high edge (count edges); an edge during a measurement is missed",
               {"count"}, [this](const Json& a) {
            if (!simulate_) throw PluginError(DM_E_STATE, "真机上 TRIG 由外部信号驱动");
            if (run_.state != "running" || !run_.manual)
                throw PluginError(DM_E_STATE, "没有在等待 TRIG 的运行：先打开“手动触发”，再点运行");
            const int n = std::max(1, std::min(a.integer("count", 1), 65535));
            advance();
            if (run_.measuring) {
                ++run_.missed;
                log("warning", "TRIG ↑ 来得太早：测量还没结束，图形不在等待循环里，这个沿被错过");
            } else {
                run_.pending += n;
                log("info", n == 1 ? "TRIG ↑（第 " + std::to_string(run_.triggers + 1) + " 次）"
                                   : "TRIG ↑ × " + std::to_string(n));
            }
            run_.triggers += n;
            return status();
        });

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

        method("spec_rows", "Specifications as written in the sheet: symbol, definition, value, comment", {}, [this](const Json&) {
            const auto sym = tdc::spec_symbols(project_->specs);
            Json rows = Json::array();
            for (auto& s : project_->specs) {
                auto it = sym.find(s.key());
                rows.push_back(Json{{"key", s.key()}, {"section", s.section}, {"symbol", s.symbol}, {"definition", s.definition},
                                    {"value", it != sym.end() ? tdc::fmt_g(it->second, 6) : std::string("无法求值")}, {"comment", s.comment}});
            }
            return kv("rows", rows);
        });

        method("set_spec_text", "Write a specification to the sheet (field: definition, comment)", {"key", "field", "value"}, [this](const Json& a) {
            return editing([&] {
                const std::string key = a.str("key"), field = a.str("field");
                const size_t dot = key.find('.');
                if (dot == std::string::npos) throw std::invalid_argument("无效的规格 " + key);
                std::string value = tdc::trim(a.str("value"));
                if (field == "definition") value = checked(value, key);
                else if (field != "comment") throw std::invalid_argument("未知字段 " + field);
                edit_sheet(project_->specs_path, [&](std::string& x) {
                    tdc::specs_set(x, key.substr(0, dot), key.substr(dot + 1), field == "definition" ? "Definition" : "Comment", value);
                });
                spec_values_.erase(key);
                return Json{{"key", key}, {"field", field}, {"value", value}};
            });
        });

        method("add_spec", "Add a specification: key = Section.Symbol, section created when new", {"key", "definition"}, [this](const Json& a) {
            return editing([&] {
                const std::string key = tdc::trim(a.str("key"));
                const size_t dot = key.find('.');
                if (dot == std::string::npos || !tdc::valid_name(key.substr(0, dot)) || !tdc::valid_name(key.substr(dot + 1)))
                    throw std::invalid_argument("规格名应写成 分组.符号，如 DC.VCC");
                const std::string value = checked(a.str("definition").empty() ? "0" : a.str("definition"), key);
                edit_sheet(project_->specs_path, [&](std::string& x) { tdc::specs_add(x, key.substr(0, dot), key.substr(dot + 1), value); });
                return kv("key", key);
            });
        });

        method("remove_spec", "Delete a specification", {"key"}, [this](const Json& a) {
            return editing([&] {
                const std::string key = a.str("key");
                const size_t dot = key.find('.');
                if (dot == std::string::npos) throw std::invalid_argument("无效的规格 " + key);
                edit_sheet(project_->specs_path, [&](std::string& x) { tdc::specs_remove(x, key.substr(0, dot), key.substr(dot + 1)); });
                spec_values_.erase(key);
                return kv("key", key);
            });
        });

        method("levels", "Pin levels with formulas evaluated against the specifications", {}, [this](const Json&) {            const auto sym = tdc::spec_symbols(specs_now());
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
                    row[k] = text;
                    try {
                        const double v = tdc::evaluate(text, sym);
                        row[key] = k[0] == 'I' ? tdc::fmt_g(v * 1e3, 6) + " mA" : tdc::fmt_g(v, 6) + " V";
                        for (auto& [s, _] : sym)
                            if (text.find(s) != std::string::npos) {
                                formulas += (formulas.empty() ? "" : "，") + std::string(k) + " = " + row[key].as_string();
                                break;
                            }
                    } catch (const std::exception&) {
                        row[key] = text;
                    }
                }
                row["formulas"] = formulas.empty() ? "—" : formulas;
                rows.push_back(row);
            }
            return Json{{"rows", rows}, {"file", tdc::base_name(project_->levels_path)}};
        });

        method("set_level", "Change a level of a pin or pin group in the levels sheet (field: Vil ... Vterm, TerminationMode)",
               {"pin", "field", "value"}, [this](const Json& a) {
            return editing([&] {
                const std::string pin = a.str("pin"), field = a.str("field");
                std::string value = tdc::trim(a.str("value"));
                if (field == "TerminationMode") {
                    if (value != "HighZ" && value != "ActiveLoad" && value != "Vterm")
                        throw std::invalid_argument("端接应为 HighZ、ActiveLoad 或 Vterm");
                } else {
                    static const std::vector<std::string> fields{"Vil", "Vih", "Vol", "Voh", "Iol", "Ioh", "Vterm"};
                    if (std::find(fields.begin(), fields.end(), field) == fields.end()) throw std::invalid_argument("未知字段 " + field);
                    value = checked(value, field);
                }
                edit_sheet(project_->levels_path, [&](std::string& x) { tdc::levels_set(x, pin, field, value); });
                log("info", "电平 " + pin + " " + field + " = " + value);
                return Json{{"pin", pin}, {"field", field}, {"value", value}};
            });
        });

        method("add_level", "Add levels for a pin or pin group, copied from the first row", {"pin"}, [this](const Json& a) {
            return editing([&] {
                if (project_->levels.empty()) throw std::invalid_argument("电平表是空的，没有可复制的电平");
                const std::string pin = a.str("pin"), from = project_->levels.front().pin;
                edit_sheet(project_->levels_path, [&](std::string& x) { tdc::levels_add(x, pin, from); });
                return kv("pin", pin);
            });
        });

        method("remove_level", "Remove the levels of a pin or pin group", {"pin"}, [this](const Json& a) {
            return editing([&] {
                const std::string pin = a.str("pin");
                edit_sheet(project_->levels_path, [&](std::string& x) { tdc::levels_remove(x, pin); });
                return kv("pin", pin);
            });
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

        method("timeset_options", "Timeset names for the selectors (all=true: with an entry for every timeset)", {"all"}, [this](const Json& a) {
            Json list = Json::array();
            if (a.boolean("all", false)) list.push_back(Json{{"label", "全部时序集"}, {"value", ""}});
            for (auto& [name, ts] : project_->timing) {
                (void)ts;
                list.push_back(Json{{"label", name}, {"value", name}});
            }
            return kv("timesets", list);
        });

        method("set_period", "Change the period of a timeset", {"name", "value"}, [this](const Json& a) {
            return editing([&] {
                const std::string name = a.str("name"), value = checked(a.str("value"), "周期", true);
                edit_sheet(sheet_of(name), [&](std::string& x) { tdc::timing_set_period(x, name, value); });
                log("info", "时序集 " + name + " 周期 = " + value);
                return Json{{"name", name}, {"value", value}};
            });
        });

        method("add_timeset", "Add a timeset, a copy of copy_of in the same timing sheet", {"name", "copy_of"}, [this](const Json& a) {
            return editing([&] {
                const std::string name = tdc::trim(a.str("name")), from = a.str("copy_of");
                if (!tdc::valid_name(name)) throw std::invalid_argument("时序集名 '" + name + "' 无效：只能用字母、数字和下划线，且不能以数字开头");
                if (project_->timing_file.count(name)) throw std::invalid_argument("已有时序集 " + name);
                edit_sheet(sheet_of(from), [&](std::string& x) { tdc::timing_add_timeset(x, name, from); });
                log("info", "新建时序集 " + name + "（复制自 " + from + "）");
                return kv("name", name);
            });
        });

        method("remove_timeset", "Delete a timeset no pattern uses", {"name"}, [this](const Json& a) {
            return editing([&] {
                const std::string name = a.str("name");
                for (auto& p : project_->patterns)
                    if (std::find(p->timesets.begin(), p->timesets.end(), name) != p->timesets.end())
                        throw std::invalid_argument("图形 " + p->file + " 还在用时序集 " + name);
                edit_sheet(sheet_of(name), [&](std::string& x) { tdc::timing_remove_timeset(x, name); });
                return kv("name", name);
            });
        });

        method("set_edge", "Change a pin's entry in a timeset (field: format, On, Data, Return, Off, Strobe)",
               {"timeset", "pin", "field", "value"}, [this](const Json& a) {
            return editing([&] {
                const std::string ts = a.str("timeset"), pin = a.str("pin"), field = a.str("field");
                std::string value = tdc::trim(a.str("value"));
                if (field == "format") {
                    const std::string fmt = value.substr(0, value.find(' '));   // "RL 归零到低" or "RL"
                    const tdc::TimingModel tm = model();
                    auto m = tm.find(ts);
                    const double half = m != tm.end() && m->second.period > 0 ? m->second.period / 2 : 1e-6;
                    edit_sheet(sheet_of(ts), [&](std::string& x) { tdc::timing_set_format(x, ts, pin, fmt, tdc::fmt_g(half * 1e9, 6) + " ns"); });
                } else {
                    value = checked(value, field);
                    edit_sheet(sheet_of(ts), [&](std::string& x) { tdc::timing_set_edge(x, ts, pin, field, value); });
                }
                log("info", "时序集 " + ts + " " + pin + " " + field + " = " + value);
                return Json{{"timeset", ts}, {"pin", pin}, {"field", field}, {"value", value}};
            });
        });

        method("add_edge", "Add an entry for a pin or group to a timeset, a copy of what it uses now", {"timeset", "pin"},
               [this](const Json& a) {
            return editing([&] {
                const std::string name = a.str("timeset"), pin = a.str("pin");
                std::string from;
                for (auto& [ts_name, ts] : project_->timing) {
                    if (ts_name != name) continue;
                    for (auto& pe : ts.edges) {
                        const auto covered = project_->expand(pe.pin);
                        if (from.empty() || std::find(covered.begin(), covered.end(), pin) != covered.end()) from = pe.pin;
                    }
                }
                edit_sheet(sheet_of(name), [&](std::string& x) { tdc::timing_add_pin(x, name, pin, from); });
                return Json{{"timeset", name}, {"pin", pin}};
            });
        });

        method("remove_edge", "Remove a pin's entry from a timeset", {"timeset", "pin"}, [this](const Json& a) {
            return editing([&] {
                const std::string name = a.str("timeset"), pin = a.str("pin");
                edit_sheet(sheet_of(name), [&](std::string& x) { tdc::timing_remove_pin(x, name, pin); });
                return Json{{"timeset", name}, {"pin", pin}};
            });
        });

        method("restore_sheets", "Put back the project files as they were before the first edit", {}, [this](const Json&) {
            return editing([&] {
                const std::string root = backup_root();
                int restored = 0;
                for (const char* ext : {".pinmap", ".specs", ".digilevels", ".digitiming", ".digipat"}) {
                    for (auto& path : tdc::find_files(root, ext)) {
                        tdc::write_file(tdc::path_join(project_->folder, path.substr(root.size() + 1)), tdc::read_file(path));
                        std::remove(path.c_str());
                        ++restored;
                    }
                }
                project_->reload_sheets();
                instrument_ready_ = false;
                ++sheet_rev_;
                log("info", "已还原 " + std::to_string(restored) + " 个工程文件");
                return kv("restored", restored);
            });
        });

        method("timeset_edges", "Drive format and edges of every pin in each timeset, as in the editor's timing sheet "
                                "(pattern=current: only the timesets and pins of the current pattern; timeset: only that one)", {"pattern", "timeset"},
               [this](const Json& a) {
            const tdc::TimingModel tm = model();
            std::vector<std::string> only_ts, only_pins;
            if (a.boolean("pattern", false)) {
                tdc::Pattern& p = pattern();
                only_ts = p.used_timesets();
                only_pins = p.pins;
            }
            auto in = [](const std::vector<std::string>& v, const std::string& x) {
                return v.empty() || std::find(v.begin(), v.end(), x) != v.end();
            };
            auto edge = [](double v) { return std::isfinite(v) ? tdc::fmt_time(v) : std::string("\xE2\x80\x94"); };
            Json rows = Json::array();
            const std::string one = a.str("timeset");
            for (auto& [name, ts] : project_->timing) {
                if (!in(only_ts, name) || (!one.empty() && name != one)) continue;
                auto mt = tm.find(name);
                if (mt == tm.end()) continue;
                const tdc::TimesetModel& m = mt->second;
                for (auto& pe : ts.edges) {
                    bool wanted = only_pins.empty();
                    for (auto& x : project_->expand(pe.pin)) wanted = wanted || in(only_pins, x);
                    if (!wanted) continue;
                    // evaluated values: those of the first pin the entry covers
                    const auto pins = project_->expand(pe.pin);
                    auto ev = m.pins.find(pins.empty() ? pe.pin : pins.front());
                    const tdc::EdgeTimes e = ev != m.pins.end() ? ev->second : tdc::EdgeTimes{};
                    std::string problems;
                    for (auto& [pin, msg] : m.problems)
                        if (pin == pe.pin) problems += (problems.empty() ? "" : "；") + msg;
                    const bool pulsed = e.format != "NR";
                    auto raw = [&pe](const char* k) { auto it = pe.edges.find(k); return it != pe.edges.end() ? it->second : std::string(); };
                    rows.push_back(Json{{"timeset", name}, {"period", tdc::fmt_time(m.period > 0 ? m.period : NaN)},
                                        {"pin", pe.pin}, {"format", tdc::format_name(pe.format)}, {"fmt", pe.format},
                                        {"on", edge(e.on)}, {"data", edge(e.data)}, {"return", pulsed ? edge(e.ret) : "\xE2\x80\x94"},
                                        {"off", edge(e.off)}, {"strobe", edge(e.strobe)},
                                        {"On", raw("On")}, {"Data", raw("Data")}, {"Return", raw("Return")},
                                        {"Off", raw("Off")}, {"Strobe", raw("Strobe")}, {"no_return", !pulsed},
                                        {"note", problems.empty() ? pe.comment : (pe.comment.empty() ? "" : pe.comment + "；") + "⚠ " + problems},
                                        {"sheet", tdc::base_name(m.sheet)}});
                }
            }
            return kv("rows", rows);
        });

        method("checks", "What the editor would flag for the current pattern (pins, levels, timesets, edges)", {},
               [this](const Json&) {
            Json rows = Json::array();
            for (auto& c : analysis()["checks"].items()) rows.push_back(Json{{"item", c}});
            return Json{{"rows", rows}, {"count", int(rows.size())},
                        {"text", rows.size() ? std::to_string(rows.size()) + " 项" : std::string("无问题")}};
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
            Json instruments = Json::array();
            for (auto& name : pm.instruments) instruments.push_back(name);
            return Json{{"rows", rows}, {"instruments", instruments}};
        });

        method("instruments", "Instruments of the pin map", {}, [this](const Json&) {
            const auto& pm = project_->pinmap;
            Json rows = Json::array();
            for (auto& name : pm.instruments) {
                int used = 0;
                for (auto& [pin, c] : pm.connections) {
                    (void)pin;
                    if (c.instrument == name) ++used;
                }
                auto ch = pm.channels.find(name);
                rows.push_back(Json{{"instrument", name}, {"name", name}, {"channels", ch != pm.channels.end() ? ch->second : 0}, {"pins", used}});
            }
            return kv("rows", rows);
        });

        method("rename_instrument", "Rename an instrument of the pin map (it must match the resource name on the real system)",
               {"instrument", "value"}, [this](const Json& a) {
            return editing([&] {
                const std::string from = a.str("instrument"), to = tdc::trim(a.str("value"));
                if (to.empty()) throw std::invalid_argument("仪器名不能为空");
                const auto& list = project_->pinmap.instruments;
                if (std::find(list.begin(), list.end(), to) != list.end()) throw std::invalid_argument("已有仪器 " + to);
                edit_sheet(project_->pinmap_path, [&](std::string& x) { tdc::pinmap_rename_instrument(x, from, to); });
                return kv("name", to);
            });
        });

        method("set_pin", "Change a pin of the pin map (field: instrument, channel, groups; groups are comma separated)",
               {"pin", "field", "value"}, [this](const Json& a) {
            return editing([&] {
                const auto& pm = project_->pinmap;
                const std::string pin = a.str("pin"), field = a.str("field"), value = tdc::trim(a.str("value"));
                if (std::find(pm.pins.begin(), pm.pins.end(), pin) == pm.pins.end()) throw std::invalid_argument("没有引脚 " + pin);
                if (field == "groups") {
                    std::string list = value;
                    const std::string wide = "\xEF\xBC\x8C";               // full-width comma
                    for (size_t at = list.find(wide); at != std::string::npos; at = list.find(wide)) list.replace(at, wide.size(), ",");
                    std::vector<std::string> groups;
                    for (size_t b = 0; b <= list.size();) {
                        const size_t e = std::min(list.find(',', b), list.size());
                        const std::string g = tdc::trim(list.substr(b, e - b));
                        if (!g.empty() && !tdc::valid_name(g)) throw std::invalid_argument("分组名 '" + g + "' 无效");
                        if (!g.empty()) groups.push_back(g);
                        b = e + 1;
                    }
                    edit_sheet(project_->pinmap_path, [&](std::string& x) { tdc::pinmap_set_groups(x, pin, groups); });
                    return Json{{"pin", pin}, {"field", field}};
                }
                auto c = pm.connections.find(pin);
                std::string instrument = c != pm.connections.end() ? c->second.instrument : pm.instruments.empty() ? std::string() : pm.instruments.front();
                int channel = c != pm.connections.end() ? c->second.channel : -1;
                if (field == "instrument") {
                    instrument = value;
                    if (std::find(pm.instruments.begin(), pm.instruments.end(), instrument) == pm.instruments.end())
                        throw std::invalid_argument("没有仪器 " + instrument);
                } else if (field == "channel") {
                    char* end = nullptr;
                    const long n = std::strtol(value.c_str(), &end, 10);
                    auto total = pm.channels.find(instrument);
                    const int limit = total != pm.channels.end() && total->second > 0 ? total->second : 1 << 16;
                    if (value.empty() || *end || n < 0 || n >= limit)
                        throw std::invalid_argument("通道应在 0 … " + std::to_string(limit - 1));
                    channel = int(n);
                } else {
                    throw std::invalid_argument("未知字段 " + field);
                }
                if (channel < 0) throw std::invalid_argument("先填通道号");
                for (auto& [other, conn] : pm.connections)
                    if (other != pin && conn.instrument == instrument && conn.channel == channel)
                        throw std::invalid_argument(instrument + " 的通道 " + std::to_string(channel) + " 已接引脚 " + other);
                edit_sheet(project_->pinmap_path, [&](std::string& x) { tdc::pinmap_set_connection(x, pin, instrument, channel); });
                log("info", "引脚 " + pin + " 接到 " + instrument + " 通道 " + std::to_string(channel));
                return Json{{"pin", pin}, {"field", field}};
            });
        });

        method("add_pin", "Add a pin to the pin map; connect it by setting its channel", {"name"}, [this](const Json& a) {
            return editing([&] {
                const std::string pin = tdc::trim(a.str("name"));
                if (!tdc::valid_name(pin)) throw std::invalid_argument("引脚名 '" + pin + "' 无效：只能用字母、数字和下划线，且不能以数字开头");
                edit_sheet(project_->pinmap_path, [&](std::string& x) { tdc::pinmap_add_pin(x, pin); });
                return kv("pin", pin);
            });
        });

        method("remove_pin", "Remove a pin no pattern, timeset or level uses", {"pin"}, [this](const Json& a) {
            return editing([&] {
                const std::string pin = a.str("pin");
                for (auto& p : project_->patterns)
                    if (std::find(p->pins.begin(), p->pins.end(), pin) != p->pins.end())
                        throw std::invalid_argument("图形 " + p->file + " 还在用引脚 " + pin);
                for (auto& lv : project_->levels)
                    if (lv.pin == pin) throw std::invalid_argument("电平表里还有 " + pin + "，先删除它的电平");
                for (auto& [name, ts] : project_->timing)
                    for (auto& pe : ts.edges)
                        if (pe.pin == pin) throw std::invalid_argument("时序集 " + name + " 里还有 " + pin + "，先删除它的边沿");
                edit_sheet(project_->pinmap_path, [&](std::string& x) { tdc::pinmap_remove_pin(x, pin); });
                return kv("pin", pin);
            });
        });

        method("sim_set_busy", "[sim] TDC conversion time before BUSY rises again", {"us"}, [this](const Json& a) {
            busy_delay_ = std::max(0.0, a.number("us", 120)) * 1e-6;
            return kv("busy_delay_us", busy_delay_ * 1e6);
        });

        // ---- generator ----
        method("pin_options", "Pins of the pin map for the selectors (groups=true: pin groups too)", {"optional", "groups"}, [this](const Json& a) {
            Json list = Json::array();
            if (a.boolean("optional", false)) list.push_back(Json{{"label", "（不用）"}, {"value", ""}});
            for (auto& p : project_->pinmap.pins) list.push_back(Json{{"label", p}, {"value", p}});
            if (a.boolean("groups", false))
                for (auto& [g, pins] : project_->pinmap.groups) {
                    (void)pins;
                    list.push_back(Json{{"label", g + "（组）"}, {"value", g}});
                }
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

    // Manual-trigger run: finish the measurement in progress, start the next one if an edge is
    // pending, and stop the simulated instrument after the last measurement.
    void advance()
    {
        Run& st = run_;
        if (st.state != "running" || !st.manual) return;
        const auto now = Clock::now();
        bool chained = false;
        for (;;) {
            if (st.measuring) {
                if (now < st.seg_end) return;
                st.measuring = false;
                chained = true;
                ++st.done;
                st.busy_time += st.active;
                if (st.done >= st.loops) {
                    try { host_call(digital_, "pattern_abort"); } catch (const PluginError&) {}
                    st.state = "done";
                    st.t_end = now;
                    st.ended = true;
                    st.pass = true;
                    log("info", st.pattern + " 完成：通过，" + std::to_string(st.done) + " 次测量" +
                                    (st.missed ? "，错过 " + std::to_string(st.missed) + " 个 TRIG 沿" : ""));
                    return;
                }
            }
            if (st.pending <= 0) return;
            --st.pending;
            st.measuring = true;
            // queued edges run back to back; a fresh edge starts the measurement now
            st.seg_start = chained ? st.seg_end : now;
            st.seg_end = st.seg_start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(st.active));
        }
    }

    Json status()
    {
        Run& st = run_;
        advance();
        if (st.state == "running" && !st.manual) {
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
            const int done = st.manual ? st.done
                             : st.state == "done" ? st.loops
                             : st.iteration > 0 ? std::min(st.loops, int(elapsed / st.iteration)) : 0;
            out["elapsed"] = elapsed;
            out["elapsed_text"] = tdc::fmt_time(elapsed);
            out["est_text"] = tdc::fmt_time(st.est);
            out["progress"] = st.state == "done" ? 100.0
                            : st.manual ? 100.0 * st.done / std::max(1, st.loops)
                                        : std::min(99.9, 100.0 * elapsed / est);
            out["loops_done"] = done;
            out["loops"] = st.loops;
            out["time_text"] = tdc::fmt_time(elapsed, 3) + " / " + tdc::fmt_time(st.est, 3);
            out["loops_text"] = std::to_string(done) + " / " + std::to_string(st.loops);
            if (st.manual)
                out["time_text"] = "测 " + tdc::fmt_time(st.busy_time, 3) + " · 等 " + tdc::fmt_time(std::max(0.0, elapsed - st.busy_time), 3);
        } else {
            out["time_text"] = "—";
            out["loops_text"] = "—";
        }
        out["manual"] = st.manual;
        out["triggers"] = st.triggers;
        out["missed"] = st.missed;
        out["waiting"] = st.state == "running" && st.manual && !st.measuring && st.pending == 0;
        out["trig_text"] = !(st.state == "running" && st.manual) ? (manual_trigger_ ? "手动触发：点运行后等待 TRIG" : "自动")
                           : st.measuring ? "测量中（第 " + std::to_string(st.done + 1) + " 次）"
                           : "等待 TRIG 上升沿（已测 " + std::to_string(st.done) + " / " + std::to_string(st.loops) + "）";
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
        // manual trigger (simulation)
        bool manual = false, measuring = false;
        double active = 0, busy_time = 0;          // one measurement / all measurements so far [s]
        int done = 0, pending = 0, triggers = 0, missed = 0;
        Clock::time_point seg_start{}, seg_end{};
    };

    std::string plugin_dir_, work_, gen_dir_, gen_template_, current_, digital_;
    std::string projects_dir_, instrument_name_, seed_pattern_, export_root_;
    std::unique_ptr<tdc::Project> project_;
    double busy_delay_ = 120e-6;
    bool simulate_ = true, instrument_ready_ = false, manual_trigger_ = false;
    unsigned sheet_rev_ = 0;                       // bumped when a sheet on disk changes
    std::map<std::string, std::string> spec_values_;
    std::map<std::string, Json> cache_;
    Json gen_last_ = Json::object();
    Run run_;
    std::mt19937 rng_;
};

DM_EXPORT_PLUGIN(TdcMeasure, kManifest)
