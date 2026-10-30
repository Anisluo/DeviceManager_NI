// sim_smu — simulated 4-channel source-measure unit.
//
// Each channel sources a voltage into a simulated load and measures voltage and
// current with gaussian noise. Compliance is modelled: when |I| would exceed the
// current limit the channel clamps to the limit and the voltage collapses.
// Loads: resistor (R), diode (Is, n), or an LDO-like regulator (Vout, Iq, R_load)
// so the page and later test tasks have something interesting to look at.
#include <dm/plugin.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <string>
#include <vector>

static const char* kManifest = R"({
  "id": "sim_smu",
  "name": "Simulated SMU",
  "version": "0.1.0",
  "type": "cpp",
  "abi": 1,
  "vendor": "DeviceManager",
  "category": "instrument",
  "capabilities": ["dcpower.source_voltage", "dcpower.measure", "dcpower.channels:4", "simulated"]
})";

namespace {

struct Channel {
    double voltage = 0.0;          // setpoint [V]
    double current_limit = 0.01;   // [A]
    bool   output = false;
    std::string load = "resistor"; // resistor | diode | ldo
    double r_load = 1000.0;        // [ohm] resistor / LDO output load
    double diode_is = 1e-12;       // [A]
    double diode_n = 1.5;
    double ldo_vout = 3.3;         // regulated output [V]
    double ldo_iq = 45e-6;         // quiescent current [A]
    double ldo_dropout = 0.18;     // [V]
    double ldo_line_reg = 4e-4;    // relative Vout change per volt of headroom [1/V]
    double ldo_r_out = 0.05;       // output resistance -> load regulation [ohm]
    bool   in_compliance = false;
};

class SimSmu : public dm::Plugin {
public:
    explicit SimSmu(const dm::Json& cfg)
        : resource_(cfg.str("resource", "SIM0")),
          channels_(size_t(std::clamp(cfg["options"].integer("channels", 4), 1, 16))),
          noise_v_(cfg["options"].number("noise_v", 20e-6)),
          noise_i_(cfg["options"].number("noise_i", 2e-9)),
          rng_(std::random_device{}()),
          t0_(std::chrono::steady_clock::now()) {

        // Optional power-on state for every channel: options.power_on = {voltage, current_limit, output, load}
        const dm::Json& po = cfg["options"]["power_on"];
        for (auto& c : channels_) {
            c.voltage = po.number("voltage", c.voltage);
            c.current_limit = po.number("current_limit", c.current_limit);
            c.output = po.boolean("output", c.output);
            c.load = po.str("load", c.load);
            c.r_load = po.number("r_load", c.r_load);
        }

        method("get_info", "Instrument identity and channel count", {},
               [this](const dm::Json&) {
                   return dm::Json{{"resource", resource_}, {"model", "SIM-SMU-4"},
                                   {"channels", int(channels_.size())}, {"simulated", true},
                                   {"v_range", 6.0}, {"i_range", 0.5}};
               });

        method("reset", "Reset all channels to 0 V, output off", {},
               [this](const dm::Json&) {
                   std::lock_guard<std::mutex> lk(m_);
                   for (auto& c : channels_) c = Channel{};
                   log("info", "reset");
                   return ok();
               });

        method("set_voltage", "Set voltage setpoint", {"channel", "voltage"},
               [this](const dm::Json& a) {
                   double v = a.number("voltage", NAN);
                   dm::require(std::isfinite(v), "voltage required");
                   dm::require(std::fabs(v) <= 6.0, "voltage out of range (+/-6 V)");
                   std::lock_guard<std::mutex> lk(m_);
                   ch(a).voltage = v;
                   return ok();
               });

        method("set_current_limit", "Set current limit", {"channel", "current_limit"},
               [this](const dm::Json& a) {
                   double i = a.number("current_limit", NAN);
                   dm::require(std::isfinite(i) && i > 0 && i <= 0.5, "current_limit must be in (0, 0.5] A");
                   std::lock_guard<std::mutex> lk(m_);
                   ch(a).current_limit = i;
                   return ok();
               });

        method("set_output", "Enable or disable the output", {"channel", "enabled"},
               [this](const dm::Json& a) {
                   std::lock_guard<std::mutex> lk(m_);
                   Channel& c = ch(a);
                   c.output = a.boolean("enabled", false);
                   emit_event(dm::Json{{"type", "state"}, {"channel", a.integer("channel", 0)},
                                       {"output", c.output}});
                   return ok();
               });

        method("set_load", "Configure the simulated load", {"channel", "load", "r_load", "ldo_vout", "ldo_iq"},
               [this](const dm::Json& a) {
                   std::lock_guard<std::mutex> lk(m_);
                   Channel& c = ch(a);
                   std::string kind = a.str("load", c.load);
                   dm::require(kind == "resistor" || kind == "diode" || kind == "ldo", "load must be resistor|diode|ldo");
                   c.load = kind;
                   c.r_load = a.number("r_load", c.r_load);
                   dm::require(c.r_load > 0, "r_load must be > 0");
                   c.ldo_vout = a.number("ldo_vout", c.ldo_vout);
                   c.ldo_iq = a.number("ldo_iq", c.ldo_iq);
                   return ok();
               });

        method("get_config", "Read back channel configuration", {"channel"},
               [this](const dm::Json& a) {
                   std::lock_guard<std::mutex> lk(m_);
                   const Channel& c = ch(a);
                   return dm::Json{{"voltage", c.voltage}, {"current_limit", c.current_limit},
                                   {"output", c.output}, {"load", c.load}, {"r_load", c.r_load}};
               });

        method("measure", "Measure voltage and current of one channel", {"channel"},
               [this](const dm::Json& a) {
                   std::lock_guard<std::mutex> lk(m_);
                   int idx = a.integer("channel", 0);
                   return measure(idx);
               });

        method("measure_all", "Measure all channels", {},
               [this](const dm::Json&) {
                   std::lock_guard<std::mutex> lk(m_);
                   dm::Json list = dm::Json::array();
                   for (int i = 0; i < int(channels_.size()); ++i) list.push_back(measure(i));
                   return dm::Json{{"channels", list}};
               });
    }

private:
    static dm::Json ok() { return dm::Json{{"ok", true}}; }

    Channel& ch(const dm::Json& a) {
        int i = a.integer("channel", 0);
        if (i < 0 || i >= int(channels_.size()))
            throw dm::PluginError(DM_E_BAD_ARGS, "channel out of range");
        return channels_[size_t(i)];
    }

    // Regulated output of the LDO model at input voltage v.
    static double ldo_out(const Channel& c, double v) {
        double headroom = std::max(0.0, v - c.ldo_vout - c.ldo_dropout);
        double reg = c.ldo_vout * (1.0 + c.ldo_line_reg * headroom);
        reg /= 1.0 + c.ldo_r_out / c.r_load;                 // drop across the output resistance
        return std::min(reg, std::max(0.0, v - c.ldo_dropout));
    }

    // Ideal (noise-free) load current at terminal voltage v.
    static double load_current(const Channel& c, double v) {
        if (c.load == "diode") {
            const double vt = 0.025852;
            return c.diode_is * (std::exp(std::min(v / (c.diode_n * vt), 80.0)) - 1.0);
        }
        if (c.load == "ldo") {
            if (v <= 0) return v / 1e6;
            double vout = ldo_out(c, v);
            // Iq rises a little in dropout, as real LDOs do.
            double iq = c.ldo_iq * (v - c.ldo_dropout < c.ldo_vout ? 2.5 : 1.0);
            return vout / c.r_load + iq;
        }
        return v / c.r_load;
    }

    dm::Json measure(int idx) {
        if (idx < 0 || idx >= int(channels_.size()))
            throw dm::PluginError(DM_E_BAD_ARGS, "channel out of range");
        Channel& c = channels_[size_t(idx)];
        double v = 0, i = 0;
        bool comp = false;
        if (c.output) {
            v = c.voltage;
            i = load_current(c, v);
            if (std::fabs(i) > c.current_limit) {
                // Bisect for the terminal voltage where the load draws exactly the limit.
                comp = true;
                double target = std::copysign(c.current_limit, i);
                double lo = 0, hi = v;
                for (int k = 0; k < 60; ++k) {
                    double mid = 0.5 * (lo + hi);
                    if (std::fabs(load_current(c, mid)) > c.current_limit) hi = mid; else lo = mid;
                }
                v = lo;
                i = target;
            }
        }
        std::normal_distribution<double> nv(0.0, noise_v_), ni(0.0, noise_i_);
        double vm = v + nv(rng_);
        double im = i + ni(rng_);
        if (comp != c.in_compliance) {
            c.in_compliance = comp;
            emit_event(dm::Json{{"type", "state"}, {"channel", idx}, {"compliance", comp}});
            log(comp ? "warning" : "info",
                "channel " + std::to_string(idx) + (comp ? " entered compliance" : " left compliance"));
        }
        // Voltage across the load's output node (the regulated rail for the LDO model);
        // noise-free so an external DMM model can add its own.
        double load_v = v;
        if (c.load == "ldo") load_v = c.output ? ldo_out(c, v) : 0.0;
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        return dm::Json{{"channel", idx}, {"voltage", vm}, {"current", im}, {"load_v", load_v},
                        {"compliance", comp}, {"output", c.output}, {"t", t}};
    }

    std::string resource_;
    std::vector<Channel> channels_;
    double noise_v_, noise_i_;
    std::mt19937_64 rng_;
    std::chrono::steady_clock::time_point t0_;
    std::mutex m_;
};

} // namespace

DM_EXPORT_PLUGIN(SimSmu, kManifest)
