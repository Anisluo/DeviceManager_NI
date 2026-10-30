// foc_motor — FOC motor controller debugging (DMFOC protocol over UART, or a simulated board).
#include <dm/plugin.hpp>

#include "dmfoc_client.hpp"
#include "foc_sim.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

static const char* kManifest = R"({
  "id": "foc_motor",
  "name": "FOC 电机调试",
  "version": "0.1.0",
  "type": "cpp",
  "abi": 1,
  "category": "instrument",
  "capabilities": ["motor.foc", "motor.pmsm", "scope", "tuning", "simulated"]
})";

namespace {

const char* MODE_NAMES[] = {"IDLE", "VF", "CURRENT", "SPEED", "POSITION"};
const char* MODE_TEXT[] = {"空闲", "开环 VF", "电流环", "速度环", "位置环"};
const char* STATE_TEXT[] = {"待机", "运行", "故障", "编码器校准中", "参数辨识中"};
const char* FAULT_TEXT[] = {"过流", "过压", "欠压", "过温", "编码器"};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

struct ParamInfo {
    uint16_t id;
    std::string name, unit, group;
    uint8_t type, flags;
    float min, max;
    std::vector<std::string> enums;
};

struct ChannelInfo {
    std::string name, unit;
};

class FocMotor : public dm::Plugin {
public:
    explicit FocMotor(const dm::Json& cfg)
    {
        const dm::Json& o = cfg["options"];
        default_port_ = o.str("port", cfg.boolean("simulate", true) ? "sim" : "");
        default_baud_ = o.integer("baud", 921600);
        if (o.boolean("auto_connect", true) && !default_port_.empty()) {
            try { connect(default_port_, default_baud_); }
            catch (const std::exception& e) { log("warning", std::string("自动连接失败：") + e.what()); }
        }

        method("list_ports", "Serial ports (plus 'sim' for the simulated board)", {}, [](const dm::Json&) {
            dm::Json names = dm::Json::array({"sim"});
            for (auto& p : SerialPort::list()) names.push_back(p);
            return dm::Json{{"ports", names}};
        });

        method("connect", "Open a port ('sim' = simulated board) and discover parameters / channels", {"port", "baud"},
               [this](const dm::Json& a) {
                   connect(a.str("port", default_port_.empty() ? "sim" : default_port_), a.integer("baud", default_baud_));
                   return info();
               });

        method("disconnect", "Close the port", {}, [this](const dm::Json&) {
            client_.reset(); transport_.reset(); sim_ = nullptr;
            params_.clear(); channels_.clear();
            return dm::Json{{"connected", false}};
        });

        method("get_info", "Firmware, parameters, channels, scope", {}, [this](const dm::Json&) { return info(); });

        method("status", "State, mode, faults and every channel value (poll this)", {}, [this](const dm::Json&) {
            return status();
        });

        method("params", "Parameter table with values (optionally one group)", {"group"}, [this](const dm::Json& a) {
            return param_table(a.str("group"));
        });

        method("param_write", "Write a parameter by name", {"name", "value"}, [this](const dm::Json& a) {
            return param_write(a.str("name"), a["value"]);
        });

        method("param_read", "Read a parameter by name", {"name"}, [this](const dm::Json& a) {
            const ParamInfo& p = find_param(a.str("name"));
            dm::Json v = read_values(p.id, 1)[0];
            return dm::Json{{"name", p.name}, {"value", v}, {"unit", p.unit}};
        });

        method("command", "enable | disable | clear_fault | estop | calibrate | identify | save | defaults", {"name", "arg"},
               [this](const dm::Json& a) { return command(a.str("name"), float(a.number("arg", 0))); });

        method("set_mode", "IDLE | VF | CURRENT | SPEED | POSITION", {"mode"}, [this](const dm::Json& a) {
            send_command(DMFOC_CMD_SET_MODE, float(mode_index(a["mode"])));
            return status();
        });

        method("set_target", "Target for the active mode: A (current), rpm (speed), ° (position), Hz (VF)", {"value"},
               [this](const dm::Json& a) {
                   send_command(DMFOC_CMD_SET_TARGET, float(a.number("value", 0)));
                   return dm::Json{{"ok", true}};
               });

        method("run", "Select a mode, set the target and enable", {"mode", "target"}, [this](const dm::Json& a) {
            send_command(DMFOC_CMD_SET_MODE, float(mode_index(a["mode"])));
            send_command(DMFOC_CMD_SET_TARGET, float(a.number("target", 0)));
            send_command(DMFOC_CMD_ENABLE, 0);
            return status();
        });

        method("scope_config", "channels 'iq,iq_ref', divider, length, trigger channel | 'none', level, edge rising|falling|both, pre_pct",
               {"channels", "divider", "length", "trigger", "level", "edge", "pre_pct"},
               [this](const dm::Json& a) {
                   scope_config(a.str("channels", "iq,iq_ref"), a.integer("divider", 1), a.integer("length", 1000),
                                a.str("trigger", "none"), float(a.number("level", 0)), a.str("edge", "rising"),
                                a.number("pre_pct", 10));
                   return dm::Json{{"ok", true}, {"length", scope_len_}};
               });

        method("scope_capture", "Arm, wait for the trigger (or force), read the capture; scope_config args apply first when given",
               {"timeout", "force", "channels", "divider", "length", "trigger", "level", "edge", "pre_pct"},
               [this](const dm::Json& a) {
                   if (a.contains("channels"))
                       scope_config(a.str("channels", "iq,iq_ref"), a.integer("divider", 1), a.integer("length", 1000),
                                    a.str("trigger", "none"), float(a.number("level", 0)), a.str("edge", "rising"),
                                    a.number("pre_pct", 10));
                   return scope_capture(a.number("timeout", 2.0), a.boolean("force", false));
               });

        method("scope_last", "The last capture (for pages)", {}, [this](const dm::Json&) { return last_capture_; });

        method("step_test", "Closed-loop step response with metrics: loop current | speed | position",
               {"loop", "amplitude", "base"},
               [this](const dm::Json& a) { return step_test(a.str("loop", "speed"), a.number("amplitude", 500), a.number("base", 0)); });

        method("calibrate", "Encoder offset / direction calibration (motor turns one pole pitch)", {}, [this](const dm::Json&) {
            send_command(DMFOC_CMD_CALIBRATE, 0);
            wait_state_not(DMFOC_ST_CALIBRATE, 5.0);
            dm::Json r = dm::Json{{"enc_offset", value_of("enc_offset")}, {"enc_dir", value_of("enc_dir")},
                                  {"calibrated", value_of("calibrated")}};
            r["result"] = r["calibrated"].as_number() ? "OK" : "FAIL";
            log(r["calibrated"].as_number() ? "info" : "error", "编码器校准" + std::string(r["calibrated"].as_number() ? "完成" : "失败"));
            return r;
        });

        method("identify", "Measure Rs and Ld; apply=true copies them into rs / ld / lq", {"apply"}, [this](const dm::Json& a) {
            send_command(DMFOC_CMD_IDENTIFY, 0);
            wait_state_not(DMFOC_ST_IDENTIFY, 5.0);
            double rs = value_of("rs_est").as_number(), ld = value_of("ld_est").as_number();
            double rs0 = value_of("rs").as_number(), ld0 = value_of("ld").as_number();
            if (a.boolean("apply", false) && rs > 0 && ld > 0) {
                param_write("rs", rs); param_write("ld", ld); param_write("lq", ld);
            }
            dm::Json rows = dm::Json::array();
            rows.push_back(dm::Json{{"quantity", "Rs"}, {"measured", rs}, {"configured", rs0}, {"unit", "Ω"},
                                    {"diff_pct", rs0 ? (rs / rs0 - 1) * 100 : 0}});
            rows.push_back(dm::Json{{"quantity", "Ld"}, {"measured", ld}, {"configured", ld0}, {"unit", "H"},
                                    {"diff_pct", ld0 ? (ld / ld0 - 1) * 100 : 0}});
            return dm::Json{{"rs", rs}, {"ld", ld}, {"rows", rows}};
        });

        method("sim_set_load", "[sim] Load torque on the shaft", {"torque"}, [this](const dm::Json& a) {
            sim().set_load(float(a.number("torque", 0)));
            return dm::Json{{"torque", a.number("torque", 0)}};
        });

        method("sim_set_vbus", "[sim] DC bus voltage", {"volts"}, [this](const dm::Json& a) {
            sim().set_vbus(float(a.number("volts", 24)));
            return dm::Json{{"vbus", a.number("volts", 24)}};
        });
    }

private:
    // ------------------------------------------------------------------ connection
    void connect(const std::string& port, int baud)
    {
        client_.reset();
        transport_.reset();
        sim_ = nullptr;
        if (port == "sim" || port.empty()) {
            auto s = std::make_unique<FocSim>(7);
            sim_ = s.get();
            transport_ = std::move(s);
        } else {
            transport_ = std::make_unique<SerialPort>(port, baud);
        }
        client_ = std::make_unique<DmfocClient>(transport_.get());
        Reader r{client_->request(DMFOC_PING, {}, 500, 3)};
        proto_ver_ = r.u8();
        uint16_t np = r.u16();
        uint8_t nc = r.u8();
        scope_capacity_ = r.u16();
        sample_rate_ = r.f32();
        fw_name_ = r.str();
        params_.clear();
        for (uint16_t i = 0; i < np; ++i) {
            Reader q{client_->request(DMFOC_PARAM_INFO, Buf().u16(i).b)};
            ParamInfo p;
            p.id = q.u16(); p.type = q.u8(); p.flags = q.u8(); p.min = q.f32(); p.max = q.f32();
            p.name = q.str(); p.unit = q.str(); p.group = q.str();
            std::string en = q.str();
            for (size_t s = 0; !en.empty();) {
                size_t e = en.find('|', s);
                p.enums.push_back(en.substr(s, e == std::string::npos ? std::string::npos : e - s));
                if (e == std::string::npos) break;
                s = e + 1;
            }
            params_.push_back(p);
        }
        channels_.clear();
        for (uint8_t i = 0; i < nc; ++i) {
            Reader q{client_->request(DMFOC_CHANNEL_INFO, Buf().u8(i).b)};
            q.u8();
            ChannelInfo c;
            c.name = q.str(); c.unit = q.str();
            channels_.push_back(c);
        }
        port_ = transport_->name();
        log("info", "已连接 " + port_ + " · " + fw_name_ + " · " + std::to_string(np) + " 个参数, " +
                    std::to_string(nc) + " 个通道");
        emit_event(dm::Json{{"type", "state"}, {"connected", true}});
    }

    DmfocClient& cl()
    {
        if (!client_) throw dm::PluginError(DM_E_STATE, "未连接控制板（先 connect）");
        return *client_;
    }

    FocSim& sim()
    {
        if (!sim_) throw dm::PluginError(DM_E_STATE, "仅仿真控制板可用");
        return *sim_;
    }

    dm::Json info()
    {
        dm::Json ch = dm::Json::array(), names = dm::Json::array();
        for (auto& c : channels_) { ch.push_back(dm::Json{{"name", c.name}, {"unit", c.unit}}); names.push_back(c.name); }
        return dm::Json{{"connected", client_ != nullptr}, {"port", port_}, {"firmware", fw_name_},
                        {"protocol", proto_ver_}, {"params", int(params_.size())}, {"channels", ch},
                        {"channel_names", names}, {"sample_rate", sample_rate_}, {"scope_capacity", scope_capacity_},
                        {"simulated", sim_ != nullptr}};
    }

    // ------------------------------------------------------------------ status
    dm::Json status()
    {
        if (!client_) return dm::Json{{"connected", false}, {"state_text", "未连接"}, {"fault", false}};
        Reader r{cl().request(DMFOC_STATUS)};
        uint8_t st = r.u8(), mode = r.u8();
        uint32_t faults = r.u32();
        uint8_t n = r.u8();
        dm::Json out{{"connected", true}, {"state", st}, {"mode", mode},
                     {"state_text", st < 5 ? STATE_TEXT[st] : "?"}, {"mode_name", mode < 5 ? MODE_NAMES[mode] : "?"},
                     {"mode_text", mode < 5 ? MODE_TEXT[mode] : "?"}, {"running", st == DMFOC_ST_RUN},
                     {"fault", (faults & 0xFFFF) != 0}};
        std::string ft;
        for (int b = 0; b < 5; ++b)
            if (faults & (1u << b)) ft += (ft.empty() ? "" : "、") + std::string(FAULT_TEXT[b]);
        out["fault_text"] = ft.empty() ? "无" : ft;
        out["calibrated"] = !(faults & DMFOC_F_NOT_CALIBRATED);
        out["hint"] = (faults & DMFOC_F_NOT_CALIBRATED) ? "编码器未校准：闭环模式前请先校准" : "";
        for (uint8_t i = 0; i < n && i < channels_.size(); ++i) out[channels_[i].name] = r.f32();
        if (sim_) out["load"] = sim_->load();
        return out;
    }

    // ------------------------------------------------------------------ parameters
    const ParamInfo& find_param(const std::string& name) const
    {
        for (auto& p : params_) if (p.name == name) return p;
        throw dm::PluginError(DM_E_BAD_ARGS, "未知参数 " + name);
    }

    dm::Json decode(const ParamInfo& p, Reader& r)
    {
        if (p.type == DMFOC_F32) return dm::Json(double(r.f32()));
        int32_t v = r.i32();
        if (p.type == DMFOC_BOOL) return dm::Json(v != 0);
        return dm::Json(v);
    }

    std::vector<dm::Json> read_values(uint16_t first, uint16_t count)
    {
        std::vector<dm::Json> out;
        for (uint16_t at = first; at < first + count;) {
            uint8_t n = uint8_t(std::min<int>(64, first + count - at));
            Reader r{cl().request(DMFOC_PARAM_READ, Buf().u16(at).u8(n).b)};
            r.u16(); r.u8();
            for (uint8_t i = 0; i < n; ++i) out.push_back(decode(params_[at + i], r));
            at = uint16_t(at + n);
        }
        return out;
    }

    dm::Json value_of(const std::string& name) { return read_values(find_param(name).id, 1)[0]; }

    dm::Json param_table(const std::string& group)
    {
        cl();
        auto values = read_values(0, uint16_t(params_.size()));
        dm::Json rows = dm::Json::array(), groups = dm::Json::array();
        std::vector<std::string> seen;
        for (size_t i = 0; i < params_.size(); ++i) {
            const ParamInfo& p = params_[i];
            if (std::find(seen.begin(), seen.end(), p.group) == seen.end()) { seen.push_back(p.group); groups.push_back(p.group); }
            if (!group.empty() && p.group != group) continue;
            dm::Json en = dm::Json::array();
            for (auto& e : p.enums) en.push_back(e);
            const char* type = p.type == DMFOC_F32 ? "float" : p.type == DMFOC_BOOL ? "bool" : p.type == DMFOC_ENUM ? "enum" : "int";
            rows.push_back(dm::Json{{"name", p.name}, {"group", p.group}, {"value", values[i]}, {"unit", p.unit},
                                    {"min", p.min}, {"max", p.max}, {"type", type}, {"readonly", bool(p.flags & DMFOC_RO)},
                                    {"persist", bool(p.flags & DMFOC_PERSIST)}, {"enums", en}});
        }
        return dm::Json{{"rows", rows}, {"groups", groups}};
    }

    dm::Json param_write(const std::string& name, const dm::Json& value)
    {
        const ParamInfo& p = find_param(name);
        Buf b;
        b.u16(p.id);
        if (p.type == DMFOC_F32) {
            b.f32(float(value.as_number()));
        } else if (p.type == DMFOC_ENUM && value.is_string()) {
            auto it = std::find(p.enums.begin(), p.enums.end(), value.as_string());
            if (it == p.enums.end()) throw dm::PluginError(DM_E_BAD_ARGS, name + " 没有选项 " + value.as_string());
            b.u32(uint32_t(it - p.enums.begin()));
        } else {
            b.u32(uint32_t(int32_t(std::lround(value.is_bool() ? (value.as_bool() ? 1 : 0) : value.as_number()))));
        }
        Reader r{cl().request(DMFOC_PARAM_WRITE, b.b)};
        r.u16();
        uint8_t st = r.u8();
        dm::Json v = decode(p, r);
        if (st == DMFOC_E_READONLY) throw dm::PluginError(DM_E_BAD_ARGS, name + " 是只读参数");
        if (st == DMFOC_E_ARG) throw dm::PluginError(DM_E_BAD_ARGS, name + "：数值无效");
        if (st == DMFOC_E_RANGE) log("warning", name + " 超出范围，已限幅为 " + v.dump());
        return dm::Json{{"name", name}, {"value", v}, {"clamped", st == DMFOC_E_RANGE}};
    }

    // ------------------------------------------------------------------ commands
    int mode_index(const dm::Json& m)
    {
        if (m.is_number()) return m.as_int();
        for (int i = 0; i < 5; ++i) if (m.as_string() == MODE_NAMES[i]) return i;
        throw dm::PluginError(DM_E_BAD_ARGS, "模式应为 IDLE / VF / CURRENT / SPEED / POSITION");
    }

    void send_command(uint8_t cmd, float arg)
    {
        Reader r{cl().request(DMFOC_COMMAND, Buf().u8(cmd).f32(arg).b)};
        r.u8();
        int8_t res = int8_t(r.u8());
        if (res != DMFOC_OK) {
            std::string why = DmfocClient::error_text(res);
            if (res == DMFOC_E_STATE) {
                dm::Json s = status();
                why += "（状态：" + s.str("state_text") + "，模式：" + s.str("mode_text");
                if (s.boolean("fault", false)) why += "，故障：" + s.str("fault_text");
                if (!s.str("hint").empty()) why += "，" + s.str("hint");
                why += "）";
            }
            throw dm::PluginError(DM_E_STATE, why);
        }
    }

    dm::Json command(const std::string& name, float arg)
    {
        static const std::pair<const char*, uint8_t> table[] = {
            {"enable", DMFOC_CMD_ENABLE}, {"disable", DMFOC_CMD_DISABLE}, {"clear_fault", DMFOC_CMD_CLEAR_FAULT},
            {"estop", DMFOC_CMD_ESTOP}, {"calibrate", DMFOC_CMD_CALIBRATE}, {"identify", DMFOC_CMD_IDENTIFY},
            {"save", DMFOC_CMD_SAVE}, {"defaults", DMFOC_CMD_DEFAULTS}};
        for (auto& t : table)
            if (name == t.first) {
                send_command(t.second, arg);
                if (name == "save") log("info", "参数已保存到控制板 Flash");
                return status();
            }
        throw dm::PluginError(DM_E_BAD_ARGS, "未知命令 " + name);
    }

    void wait_state_not(int st, double timeout)
    {
        auto t0 = std::chrono::steady_clock::now();
        while (true) {
            sleep_ms(50);
            dm::Json s = status();
            if (s.integer("state", -1) != st) return;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout)
                throw dm::PluginError(DM_E_HW, "等待超时（控制板仍在执行）");
        }
    }

    // ------------------------------------------------------------------ scope
    int channel_index(const std::string& n) const
    {
        for (size_t i = 0; i < channels_.size(); ++i) if (channels_[i].name == n) return int(i);
        throw dm::PluginError(DM_E_BAD_ARGS, "未知通道 " + n);
    }

    void scope_config(const std::string& chs, int divider, int length, const std::string& trigger, float level,
                      const std::string& edge, double pre_pct)
    {
        cl();
        std::vector<int> ids;
        for (size_t s = 0; s <= chs.size();) {
            size_t e = chs.find(',', s);
            std::string n = chs.substr(s, e == std::string::npos ? std::string::npos : e - s);
            n.erase(0, n.find_first_not_of(' '));
            n.erase(n.find_last_not_of(' ') + 1);
            if (!n.empty()) ids.push_back(channel_index(n));
            if (e == std::string::npos) break;
            s = e + 1;
        }
        dm::require(!ids.empty() && ids.size() <= DMFOC_SCOPE_MAX_CH, "通道数应为 1…8");
        Buf b;
        b.u8(uint8_t(ids.size()));
        for (int i : ids) b.u8(uint8_t(i));
        uint16_t len = uint16_t(std::clamp(length, 2, 60000));
        b.u16(uint16_t(std::clamp(divider, 1, 60000))).u16(len);
        b.u8(trigger == "none" || trigger.empty() ? 0xFF : uint8_t(channel_index(trigger)));
        b.u8(edge == "falling" ? 1 : edge == "both" ? 2 : 0);
        b.f32(level);
        b.u16(uint16_t(std::clamp(pre_pct, 0.0, 99.0) / 100.0 * len));
        Reader r{cl().request(DMFOC_SCOPE_CONFIG, b.b)};
        uint8_t st = r.u8();
        if (st != DMFOC_OK) throw dm::PluginError(DM_E_BAD_ARGS, "示波器配置被拒绝：" + DmfocClient::error_text(st));
        scope_len_ = r.u16();
        scope_ids_ = ids;
    }

    void scope_arm(bool force)
    {
        Reader r{cl().request(DMFOC_SCOPE_ARM, Buf().u8(force ? 1 : 0).b)};
        if (r.u8() != DMFOC_OK) throw dm::PluginError(DM_E_STATE, "示波器未配置");
    }

    dm::Json scope_wait_read(double timeout)
    {
        auto t0 = std::chrono::steady_clock::now();
        uint16_t len = 0, pre = 0;
        uint8_t nch = 0;
        float dt = 0;
        while (true) {
            Reader r{cl().request(DMFOC_SCOPE_STATUS)};
            uint8_t st = r.u8();
            len = r.u16(); nch = r.u8(); pre = r.u16(); dt = r.f32();
            if (st == 3) break;
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > timeout)
                throw dm::PluginError(DM_E_HW, st == 1 ? "示波器等待触发超时（触发电平 / 通道？可用 force）" : "示波器采集超时");
            sleep_ms(10);
        }
        std::vector<std::vector<double>> data(nch);
        uint16_t per = uint16_t(std::max(1, std::min(255, int((DMFOC_MAX_PAYLOAD - 3) / (4 * nch)))));
        for (uint16_t off = 0; off < len;) {
            Reader r{cl().request(DMFOC_SCOPE_READ, Buf().u16(off).u8(uint8_t(std::min<int>(per, len - off))).b)};
            r.u16();
            uint8_t cnt = r.u8();
            for (uint8_t s = 0; s < cnt; ++s)
                for (uint8_t c = 0; c < nch; ++c) data[c].push_back(r.f32());
            if (!cnt) break;
            off = uint16_t(off + cnt);
        }
        dm::Json t = dm::Json::array();
        for (uint16_t i = 0; i < len; ++i) t.push_back((double(i) - pre) * dt * 1000.0);     // ms from the trigger
        dm::Json chs = dm::Json::array();
        for (uint8_t c = 0; c < nch && c < scope_ids_.size(); ++c) {
            dm::Json d = dm::Json::array();
            for (double v : data[c]) d.push_back(v);
            const ChannelInfo& ci = channels_[size_t(scope_ids_[c])];
            chs.push_back(dm::Json{{"name", ci.name}, {"unit", ci.unit}, {"data", d}});
        }
        last_capture_ = dm::Json{{"t", t}, {"channels", chs}, {"dt_ms", dt * 1000.0}, {"length", len},
                                 {"pre", pre}, {"captured", true}};
        return last_capture_;
    }

    dm::Json scope_capture(double timeout, bool force)
    {
        if (scope_len_ == 0) scope_config("iq,iq_ref,speed", 1, 1000, "none", 0, "rising", 10);
        scope_arm(force);
        return scope_wait_read(timeout);
    }

    // ------------------------------------------------------------------ step response
    static dm::Json metrics(const std::vector<double>& t, const std::vector<double>& y, size_t step_at, double r0, double r1)
    {
        double amp = r1 - r0;
        dm::Json m{{"target", r1}, {"start", r0}};
        if (std::fabs(amp) < 1e-9 || step_at >= y.size()) return m;
        auto frac = [&](double v) { return (v - r0) / amp; };
        double t10 = NAN, t90 = NAN, peak = 0;
        size_t settle_idx = step_at;
        for (size_t i = step_at; i < y.size(); ++i) {
            double f = frac(y[i]);
            if (std::isnan(t10) && f >= 0.1) t10 = t[i];
            if (std::isnan(t90) && f >= 0.9) t90 = t[i];
            peak = std::max(peak, f);
            if (std::fabs(f - 1.0) > 0.02) settle_idx = i + 1;
        }
        size_t tail = y.size() - std::max<size_t>(1, (y.size() - step_at) / 10);
        double final_v = 0;
        for (size_t i = tail; i < y.size(); ++i) final_v += y[i];
        final_v /= double(y.size() - tail);
        m["rise_ms"] = std::isnan(t10) || std::isnan(t90) ? dm::Json() : dm::Json(t90 - t10);
        m["overshoot_pct"] = std::max(0.0, (peak - 1.0) * 100.0);
        m["settle_ms"] = settle_idx < y.size() ? dm::Json(t[settle_idx] - t[step_at]) : dm::Json();
        m["final"] = final_v;
        m["error"] = final_v - r1;
        return m;
    }

    dm::Json step_test(const std::string& loop, double amp, double base)
    {
        struct Setup { const char* mode; const char* chs; const char* fb; const char* ref; int div; int len; int settle_ms; };
        Setup s;
        if (loop == "current")       s = {"CURRENT", "iq,iq_ref,id,uq", "iq", "iq_ref", 1, 400, 100};
        else if (loop == "speed")    s = {"SPEED", "speed,speed_ref,iq", "speed", "speed_ref", 10, 600, 400};
        else if (loop == "position") s = {"POSITION", "pos,pos_ref,speed", "pos", "pos_ref", 20, 750, 600};
        else throw dm::PluginError(DM_E_BAD_ARGS, "loop 应为 current / speed / position");

        dm::Json accel;
        if (loop == "speed") { accel = value_of("accel"); param_write("accel", 200000.0); }   // a real step, no ramp
        try {
            send_command(DMFOC_CMD_SET_MODE, float(mode_index(dm::Json(s.mode))));
            double start = base;
            if (loop == "position") start = status().number("pos", 0) + base;
            send_command(DMFOC_CMD_SET_TARGET, float(start));
            send_command(DMFOC_CMD_ENABLE, 0);
            sleep_ms(s.settle_ms);
            scope_config(s.chs, s.div, s.len, s.ref, float(start + amp / 2), amp >= 0 ? "rising" : "falling", 10);
            scope_arm(false);
            // The trigger is only looked for once the pre-trigger part of the buffer is full: wait for it.
            int pre_ms = int(std::ceil(0.10 * scope_len_ * s.div * 1000.0 / (sample_rate_ > 0 ? sample_rate_ : 20000.0)));
            sleep_ms(pre_ms + 20);
            send_command(DMFOC_CMD_SET_TARGET, float(start + amp));
            dm::Json cap = scope_wait_read(3.0);
            send_command(DMFOC_CMD_SET_TARGET, float(start));                  // back to the operating point

            std::vector<double> t, y;
            for (auto& v : cap["t"].items()) t.push_back(v.as_number());
            for (auto& c : cap["channels"].items())
                if (c.str("name") == s.fb) for (auto& v : c["data"].items()) y.push_back(v.as_number());
            size_t step_at = size_t(cap.integer("pre", 0));
            double sign = amp >= 0 ? 1 : -1;
            // metrics on a positive-going signal
            std::vector<double> ys(y.size());
            for (size_t i = 0; i < y.size(); ++i) ys[i] = sign * y[i];
            dm::Json m = metrics(t, ys, step_at, sign * start, sign * (start + amp));
            if (sign < 0) { m["target"] = start + amp; m["start"] = start; m["final"] = -m.number("final", 0); m["error"] = -m.number("error", 0); }
            dm::Json rows = dm::Json::array();
            auto row = [&](const char* k, const char* label, const char* unit) {
                rows.push_back(dm::Json{{"metric", label}, {"value", m[k]}, {"unit", unit}});
            };
            const char* u = loop == "current" ? "A" : loop == "speed" ? "rpm" : "°";
            row("rise_ms", "上升时间 10–90%", "ms");
            row("overshoot_pct", "超调", "%");
            row("settle_ms", "调节时间 ±2%", "ms");
            row("error", "稳态误差", u);
            cap["metrics"] = m;
            cap["rows"] = rows;
            cap["loop"] = loop;
            last_capture_ = cap;
            if (loop == "speed") param_write("accel", accel);
            return cap;
        } catch (...) {
            if (loop == "speed" && accel.is_number()) { try { param_write("accel", accel); } catch (...) {} }
            throw;
        }
    }

    std::unique_ptr<Transport> transport_;
    std::unique_ptr<DmfocClient> client_;
    FocSim* sim_ = nullptr;
    std::string default_port_, port_, fw_name_;
    int default_baud_ = 921600;
    int proto_ver_ = 0;
    int scope_capacity_ = 0;
    double sample_rate_ = 0;
    std::vector<ParamInfo> params_;
    std::vector<ChannelInfo> channels_;
    std::vector<int> scope_ids_;
    int scope_len_ = 0;
    dm::Json last_capture_ = dm::Json{{"captured", false}};
};

}  // namespace

DM_EXPORT_PLUGIN(FocMotor, kManifest)
