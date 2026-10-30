#include "foc_sim.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr float PI_F = 3.14159265358979f;
constexpr float TWO_PI = 2.0f * PI_F;
constexpr float RPM_PER_RADS = 60.0f / TWO_PI;
constexpr float SQRT3 = 1.7320508f;

float wrap_2pi(float a) { a = std::fmod(a, TWO_PI); return a < 0 ? a + TWO_PI : a; }
float wrap_pi(float a) { a = wrap_2pi(a + PI_F); return a - PI_F; }
float clampf(float v, float lim) { return std::max(-lim, std::min(lim, v)); }

}  // namespace

FocSim::FocSim(unsigned seed) : rng_(seed)
{
    std::uniform_real_distribution<float> u(0.0f, TWO_PI);
    m_.enc_mech_offset = u(rng_);           // unknown until the encoder is calibrated
    m_.thm = u(rng_) / m_.pp;
    defaults_ = c_;
    saved_ = c_;
    build_tables();

    scope_buf_.resize(16384);
    dmfoc_config_t cfg{};
    cfg.fw_name = "DMFOC sim · PMSM 7pp 24V";
    cfg.params = params_.data();
    cfg.n_params = uint16_t(params_.size());
    cfg.channels = channels_.data();
    cfg.n_channels = uint8_t(channels_.size());
    cfg.sample_rate_hz = 1.0f / TS;
    cfg.scope_buf = scope_buf_.data();
    cfg.scope_capacity = uint16_t(scope_buf_.size());
    cfg.user = this;
    cfg.tx = [](const uint8_t* d, uint16_t n, void* user) {
        auto* self = static_cast<FocSim*>(user);
        self->out_.insert(self->out_.end(), d, d + n);      // called with mu_ held
        self->out_cv_.notify_all();
    };
    cfg.command = [](uint8_t cmd, float arg, void* user) { return static_cast<FocSim*>(user)->command(cmd, arg); };
    cfg.state = [](uint8_t* st, uint8_t* mode, uint32_t* faults, void* user) {
        auto* self = static_cast<FocSim*>(user);
        *st = self->c_.state;
        *mode = uint8_t(self->c_.mode);
        *faults = self->c_.faults | (self->c_.calibrated ? 0u : DMFOC_F_NOT_CALIBRATED);
    };
    dmfoc_init(&proto_, &cfg);

    // Real-time pacing: run as many 50 µs control periods as wall-clock time has advanced.
    thread_ = std::thread([this] {
        using clk = std::chrono::steady_clock;
        auto last = clk::now();
        double pending = 0.0;
        while (run_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            auto now = clk::now();
            pending += std::chrono::duration<double>(now - last).count();
            last = now;
            int steps = int(pending / TS);
            if (steps > 2000) { steps = 2000; pending = 0.0; }    // don't try to catch up after a stall
            else pending -= steps * double(TS);
            std::lock_guard<std::mutex> lk(mu_);
            for (int i = 0; i < steps; ++i) {
                step();
                dmfoc_scope_sample(&proto_);
            }
        }
    });
}

FocSim::~FocSim()
{
    run_ = false;
    if (thread_.joinable()) thread_.join();
}

// ---------------------------------------------------------------- transport

void FocSim::write(const uint8_t* data, size_t n)
{
    std::lock_guard<std::mutex> lk(mu_);
    while (n) {
        uint16_t chunk = uint16_t(std::min<size_t>(n, 4096));
        dmfoc_rx(&proto_, data, chunk);
        data += chunk;
        n -= chunk;
    }
}

size_t FocSim::read(uint8_t* buf, size_t max, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(mu_);
    out_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [this] { return !out_.empty(); });
    size_t n = std::min(max, out_.size());
    std::copy(out_.begin(), out_.begin() + long(n), buf);
    out_.erase(out_.begin(), out_.begin() + long(n));
    return n;
}

void FocSim::set_load(float t) { std::lock_guard<std::mutex> lk(mu_); load_tl_ = t; }
void FocSim::set_vbus(float v) { std::lock_guard<std::mutex> lk(mu_); m_.vbus = v; }

// ---------------------------------------------------------------- tables

void FocSim::build_tables()
{
    auto F = [this](const char* n, const char* u, const char* g, float* p, float lo, float hi, uint8_t fl = DMFOC_PERSIST) {
        params_.push_back({n, u, g, DMFOC_F32, fl, lo, hi, p, nullptr});
    };
    auto I = [this](const char* n, const char* u, const char* g, int32_t* p, uint8_t type, float lo, float hi,
                    uint8_t fl = DMFOC_PERSIST, const char* en = nullptr) {
        params_.push_back({n, u, g, type, fl, lo, hi, p, en});
    };
    F("cur_kp", "V/A", "电流环", &c_.cur_kp, 0, 50);
    F("cur_ki", "V/(A·s)", "电流环", &c_.cur_ki, 0, 50000);
    I("decouple", "", "电流环", &c_.decouple, DMFOC_BOOL, 0, 1);
    F("spd_kp", "A/rpm", "速度环", &c_.spd_kp, 0, 1);
    F("spd_ki", "A/(rpm·s)", "速度环", &c_.spd_ki, 0, 20);
    F("spd_lpf_hz", "Hz", "速度环", &c_.spd_lpf_hz, 10, 3000);
    F("accel", "rpm/s", "速度环", &c_.accel, 10, 200000);
    F("pos_kp", "rpm/°", "位置环", &c_.pos_kp, 0, 100);
    F("i_max", "A", "限制", &c_.i_max, 0.1f, 15);
    F("speed_max", "rpm", "限制", &c_.speed_max, 10, 6000);
    F("oc_limit", "A", "保护", &c_.oc_limit, 1, 30);
    F("ov_limit", "V", "保护", &c_.ov_limit, 10, 60);
    F("uv_limit", "V", "保护", &c_.uv_limit, 5, 40);
    F("ot_limit", "°C", "保护", &c_.ot_limit, 40, 150);
    I("pole_pairs", "", "电机", &c_.pole_pairs, DMFOC_I32, 1, 50);
    F("rs", "Ω", "电机", &c_.rs, 0.001f, 100);
    F("ld", "H", "电机", &c_.ld, 1e-6f, 1);
    F("lq", "H", "电机", &c_.lq, 1e-6f, 1);
    F("flux", "Wb", "电机", &c_.flux, 0, 1);
    F("enc_offset", "rad", "编码器", &c_.enc_offset, 0, TWO_PI);
    I("enc_dir", "", "编码器", &c_.enc_dir, DMFOC_ENUM, 0, 1, DMFOC_PERSIST, "NORMAL|REVERSED");
    I("calibrated", "", "编码器", &c_.calibrated, DMFOC_BOOL, 0, 1, DMFOC_RO);
    F("cal_current", "A", "编码器", &c_.cal_current, 0.5f, 10);
    F("vf_freq", "Hz", "开环 VF", &c_.vf_freq, -500, 500, 0);
    F("vf_volt", "V", "开环 VF", &c_.vf_volt, 0, 12, 0);
    F("rs_est", "Ω", "辨识结果", &c_.rs_est, 0, 0, DMFOC_RO);
    F("ld_est", "H", "辨识结果", &c_.ld_est, 0, 0, DMFOC_RO);
    I("mode", "", "状态", &c_.mode, DMFOC_ENUM, 0, 4, DMFOC_RO, "IDLE|VF|CURRENT|SPEED|POSITION");

    auto C = [this](const char* n, const char* u, float* p) { channels_.push_back({n, u, p}); };
    C("id", "A", &ch_.id);            C("iq", "A", &ch_.iq);
    C("id_ref", "A", &ch_.id_ref);    C("iq_ref", "A", &ch_.iq_ref);
    C("ud", "V", &ch_.ud);            C("uq", "V", &ch_.uq);
    C("speed", "rpm", &ch_.speed);    C("speed_ref", "rpm", &ch_.speed_ref);
    C("pos", "°", &ch_.pos);          C("pos_ref", "°", &ch_.pos_ref);
    C("theta_e", "°", &ch_.theta_e);
    C("ia", "A", &ch_.ia);            C("ib", "A", &ch_.ib);            C("ic", "A", &ch_.ic);
    C("vbus", "V", &ch_.vbus);        C("temp", "°C", &ch_.temp);       C("torque", "N·m", &ch_.torque);
}

// ---------------------------------------------------------------- plant

float FocSim::encoder_mech() const
{
    const float counts = 16384.0f;                          // 14-bit absolute encoder
    float a = wrap_2pi(m_.thm + m_.enc_mech_offset);
    return std::floor(a / TWO_PI * counts) * (TWO_PI / counts);
}

void FocSim::plant(float ua, float ub, float dt)
{
    Plant& m = m_;
    float th = m.pp * m.thm;
    float co = std::cos(th), si = std::sin(th);
    if (m.enabled) {
        float ud = ua * co + ub * si, uq = -ua * si + ub * co;
        const int sub = 2;
        float h = dt / sub;
        for (int k = 0; k < sub; ++k) {
            float we = m.pp * m.wm;
            float did = (ud - m.rs * m.id + we * m.lq * m.iq) / m.ld;
            float diq = (uq - m.rs * m.iq - we * m.ld * m.id - we * m.flux) / m.lq;
            m.id += did * h;
            m.iq += diq * h;
        }
    } else {
        m.id = m.iq = 0.0f;                                 // bridge off: no current path at these speeds
    }
    float te = 1.5f * m.pp * (m.flux * m.iq + (m.ld - m.lq) * m.id * m.iq);
    float tl = load_tl_ + m.b * m.wm + 0.002f * std::tanh(m.wm * 2.0f);
    m.wm += (te - tl) / m.j * dt;
    m.thm += m.wm * dt;
    float p = 1.5f * m.rs * (m.id * m.id + m.iq * m.iq);
    m.temp += (p - (m.temp - 25.0f) / 2.0f) / 100.0f * dt;   // Rth 2 K/W, Cth 100 J/K
    ch_.torque = te;
}

void FocSim::enable_outputs(bool on)
{
    m_.enabled = on;
    c_.id_i = c_.iq_i = c_.spd_i = 0.0f;
}

// ---------------------------------------------------------------- controller (one 50 µs period)

void FocSim::step()
{
    Ctrl& c = c_;
    // ---- encoder: mechanical angle, speed, multi-turn position, electrical angle
    float dir = c.enc_dir ? -1.0f : 1.0f;
    float mech = wrap_2pi(dir * encoder_mech());
    if (c.first) { c.last_mech = mech; c.first = false; }
    float dm = wrap_pi(mech - c.last_mech);
    c.last_mech = mech;
    c.pos_acc += dm;
    float rpm_raw = dm / TS * RPM_PER_RADS;
    float alpha = 1.0f - std::exp(-TWO_PI * c.spd_lpf_hz * TS);
    c.speed_f += (rpm_raw - c.speed_f) * alpha;
    float theta_e = wrap_2pi(c.pole_pairs * mech - c.enc_offset);

    // ---- current sensing (phase currents of the true machine + noise)
    float tht = m_.pp * m_.thm;
    float ial = m_.id * std::cos(tht) - m_.iq * std::sin(tht);
    float ibe = m_.id * std::sin(tht) + m_.iq * std::cos(tht);
    float ia = ial + noise_(rng_), ib = -0.5f * ial + 0.5f * SQRT3 * ibe + noise_(rng_);
    float ic = -ia - ib;
    ial = ia;
    ibe = (ia + 2.0f * ib) / SQRT3;

    bool open_loop_angle = c.state == DMFOC_ST_CALIBRATE || c.state == DMFOC_ST_IDENTIFY
                           || (c.state == DMFOC_ST_RUN && c.mode == DMFOC_MODE_VF);
    float th = open_loop_angle ? c.theta_ol : theta_e;
    float co = std::cos(th), si = std::sin(th);
    float id_m = ial * co + ibe * si, iq_m = -ial * si + ibe * co;

    // ---- protection
    float is = std::hypot(id_m, iq_m);
    uint32_t f = 0;
    if (is > c.oc_limit) f |= DMFOC_F_OVERCURRENT;
    if (m_.vbus > c.ov_limit) f |= DMFOC_F_OVERVOLTAGE;
    if (m_.vbus < c.uv_limit && m_.enabled) f |= DMFOC_F_UNDERVOLTAGE;
    if (m_.temp > c.ot_limit) f |= DMFOC_F_OVERTEMP;
    if (f) {
        c.faults |= f;
        c.state = DMFOC_ST_FAULT;
        enable_outputs(false);
    }

    // ---- references
    float id_ref = 0, iq_ref = 0, ud = 0, uq = 0;
    bool use_pi = false, raw_voltage = false;
    float vmax = m_.vbus / SQRT3 * 0.96f;
    float pos_deg = c.pos_acc * 180.0f / PI_F;
    float t = (c.t_phase += TS);

    if (c.state == DMFOC_ST_RUN) {
        switch (c.mode) {
        case DMFOC_MODE_VF:
            c.theta_ol = wrap_2pi(c.theta_ol + TWO_PI * c.vf_freq * TS);
            uq = std::min(c.vf_volt, vmax);
            raw_voltage = true;
            break;
        case DMFOC_MODE_CURRENT:
            iq_ref = clampf(c.target_current, c.i_max);
            use_pi = true;
            break;
        case DMFOC_MODE_POSITION:
        case DMFOC_MODE_SPEED: {
            float sref;
            if (c.mode == DMFOC_MODE_POSITION) {
                sref = clampf(c.pos_kp * (c.target_pos - pos_deg), c.speed_max);
                c.speed_ramp = sref;
            } else {
                float tgt = clampf(c.target_speed, c.speed_max);
                float stepv = c.accel * TS;
                c.speed_ramp += clampf(tgt - c.speed_ramp, stepv);
                sref = c.speed_ramp;
            }
            float e = sref - c.speed_f;
            float iq_u = c.spd_kp * e + c.spd_i;
            if (std::fabs(iq_u) < c.i_max || iq_u * e < 0)          // conditional integration (anti-windup)
                c.spd_i = clampf(c.spd_i + c.spd_ki * e * TS, c.i_max);
            iq_ref = clampf(c.spd_kp * e + c.spd_i, c.i_max);
            use_pi = true;
            break;
        }
        default:
            break;
        }
    } else if (c.state == DMFOC_ST_CALIBRATE) {
        // 0–0.4 s align to θ=0, 0.4–0.9 s turn one electrical revolution, 0.9–1.2 s hold, then compute
        id_ref = c.cal_current;
        use_pi = true;
        if (c.phase == 0 && t >= 0.4f) { c.cal_a = encoder_mech(); c.phase = 1; c.t_phase = 0; }
        else if (c.phase == 1) {
            c.theta_ol = wrap_2pi(TWO_PI * std::min(1.0f, t / 0.5f));
            if (t >= 0.5f) { c.phase = 2; c.t_phase = 0; c.theta_ol = 0.0f; }
        } else if (c.phase == 2 && t >= 0.3f) {
            float b = encoder_mech();
            float delta = wrap_pi(b - c.cal_a);              // + one pole pitch if the encoder counts forward
            c.enc_dir = delta >= 0 ? 0 : 1;
            float mech_b = wrap_2pi((c.enc_dir ? -1.0f : 1.0f) * b);
            c.enc_offset = wrap_2pi(c.pole_pairs * mech_b);  // the rotor sits at electrical angle 0
            c.calibrated = 1;
            c.first = true;
            c.state = DMFOC_ST_IDLE;
            enable_outputs(false);
            use_pi = false;
            id_ref = 0;
        }
    } else if (c.state == DMFOC_ST_IDENTIFY) {
        // Rs: two DC currents on the aligned d axis; Ld: di/dt of a short voltage step
        const float i1 = 0.2f * c.i_max, i2 = 0.4f * c.i_max, vstep = 3.0f;
        c.theta_ol = 0.0f;
        if (c.phase == 0) {                                  // align + let the rotor settle, then average
            id_ref = i1; use_pi = true;
            if (t > 0.5f) { c.acc_a += c.id_i; c.acc_b += id_m; ++c.n_acc; }
            if (t >= 0.8f) { c.phase = 1; c.t_phase = 0; c.u1 = c.acc_a / c.n_acc; c.i1 = c.acc_b / c.n_acc;
                             c.acc_a = c.acc_b = 0; c.n_acc = 0; }
        } else if (c.phase == 1) {
            id_ref = i2; use_pi = true;
            if (t > 0.2f) { c.acc_a += c.id_i; c.acc_b += id_m; ++c.n_acc; }
            if (t >= 0.5f) {
                float u2 = c.acc_a / c.n_acc, j2 = c.acc_b / c.n_acc;
                c.rs_est = (u2 - c.u1) / (j2 - c.i1);       // the integrator holds the steady-state ud
                c.phase = 2; c.t_phase = 0; c.acc_a = c.acc_b = 0; c.n_acc = 0;
            }
        } else if (c.phase == 2) {
            id_ref = 0; use_pi = true;                       // let the current decay
            if (t >= 0.1f) { c.phase = 3; c.t_phase = 0; c.id_i = c.iq_i = 0; }
        } else if (c.phase == 3) {
            raw_voltage = true; ud = vstep;
            int k = int(std::lround(t / TS));
            if (k == 2) c.acc_a = id_m;
            if (k == 12) {
                float didt = (id_m - c.acc_a) / (10 * TS);
                c.ld_est = didt > 0 ? (vstep - c.rs_est * 0.5f * (id_m + c.acc_a)) / didt : 0.0f;
                c.state = DMFOC_ST_IDLE;
                enable_outputs(false);
                raw_voltage = false; ud = 0;
            }
        }
    }

    // ---- current loop
    if (use_pi) {
        float ed = id_ref - id_m, eq = iq_ref - iq_m;
        ud = c.cur_kp * ed + c.id_i;
        uq = c.cur_kp * eq + c.iq_i;
        if (c.decouple && c.state == DMFOC_ST_RUN) {
            float we = c.pole_pairs * c.speed_f / RPM_PER_RADS;
            ud -= we * c.lq * iq_m;
            uq += we * (c.ld * id_m + c.flux);
        }
        float mag = std::hypot(ud, uq);
        bool sat = mag > vmax;
        if (sat) { ud *= vmax / mag; uq *= vmax / mag; }
        if (!sat) {
            c.id_i = clampf(c.id_i + c.cur_ki * ed * TS, vmax);
            c.iq_i = clampf(c.iq_i + c.cur_ki * eq * TS, vmax);
        }
    } else if (!raw_voltage) {
        ud = uq = 0;
    }

    // ---- inverse Park -> plant
    float ua = ud * co - uq * si, ub = ud * si + uq * co;
    plant(ua, ub, TS);

    // ---- channels
    ch_.id = id_m; ch_.iq = iq_m; ch_.id_ref = id_ref; ch_.iq_ref = iq_ref;
    ch_.ud = ud; ch_.uq = uq;
    ch_.speed = c.speed_f;
    ch_.speed_ref = (c.mode == DMFOC_MODE_SPEED || c.mode == DMFOC_MODE_POSITION) && c.state == DMFOC_ST_RUN ? c.speed_ramp : 0.0f;
    ch_.pos = pos_deg;
    ch_.pos_ref = c.mode == DMFOC_MODE_POSITION ? c.target_pos : pos_deg;
    ch_.theta_e = theta_e * 180.0f / PI_F;
    ch_.ia = ia; ch_.ib = ib; ch_.ic = ic;
    ch_.vbus = m_.vbus; ch_.temp = m_.temp;
}

// ---------------------------------------------------------------- commands

int8_t FocSim::command(uint8_t cmd, float arg)
{
    Ctrl& c = c_;
    auto start_routine = [&](uint8_t st) -> int8_t {
        if (c.state == DMFOC_ST_RUN || (c.faults & 0xFFFF)) return DMFOC_E_STATE;
        c.state = st; c.phase = 0; c.t_phase = 0; c.theta_ol = 0;
        c.acc_a = c.acc_b = 0; c.n_acc = 0;
        enable_outputs(true);
        return DMFOC_OK;
    };
    switch (cmd) {
    case DMFOC_CMD_ENABLE:
        if (c.faults & 0xFFFF) return DMFOC_E_STATE;
        if (c.mode == DMFOC_MODE_IDLE) return DMFOC_E_STATE;
        if (c.mode != DMFOC_MODE_VF && !c.calibrated) return DMFOC_E_STATE;
        if (c.state == DMFOC_ST_RUN) return DMFOC_OK;
        c.speed_ramp = c.speed_f;
        if (c.mode == DMFOC_MODE_POSITION) c.target_pos = c.pos_acc * 180.0f / PI_F;
        c.state = DMFOC_ST_RUN;
        enable_outputs(true);
        return DMFOC_OK;
    case DMFOC_CMD_DISABLE:
    case DMFOC_CMD_ESTOP:
        if (c.state != DMFOC_ST_FAULT) c.state = DMFOC_ST_IDLE;
        enable_outputs(false);
        return DMFOC_OK;
    case DMFOC_CMD_CLEAR_FAULT:
        c.faults = 0;
        if (c.state == DMFOC_ST_FAULT) c.state = DMFOC_ST_IDLE;
        return DMFOC_OK;
    case DMFOC_CMD_SET_MODE: {
        int mode = int(std::lround(arg));
        if (mode < DMFOC_MODE_IDLE || mode > DMFOC_MODE_POSITION) return DMFOC_E_ARG;
        if (c.state == DMFOC_ST_RUN && mode != DMFOC_MODE_VF && !c.calibrated) return DMFOC_E_STATE;
        c.mode = mode;
        c.id_i = c.iq_i = c.spd_i = 0;
        c.speed_ramp = c.speed_f;
        c.target_pos = c.pos_acc * 180.0f / PI_F;           // hold where we are
        if (mode == DMFOC_MODE_IDLE && c.state == DMFOC_ST_RUN) { c.state = DMFOC_ST_IDLE; enable_outputs(false); }
        return DMFOC_OK;
    }
    case DMFOC_CMD_SET_TARGET:
        switch (c.mode) {
        case DMFOC_MODE_VF:       c.vf_freq = arg; break;
        case DMFOC_MODE_CURRENT:  c.target_current = arg; break;
        case DMFOC_MODE_SPEED:    c.target_speed = arg; break;
        case DMFOC_MODE_POSITION: c.target_pos = arg; break;
        default: return DMFOC_E_STATE;
        }
        return DMFOC_OK;
    case DMFOC_CMD_CALIBRATE:
        return start_routine(DMFOC_ST_CALIBRATE);
    case DMFOC_CMD_IDENTIFY:
        return start_routine(DMFOC_ST_IDENTIFY);
    case DMFOC_CMD_SAVE:
        persist_save();
        return DMFOC_OK;
    case DMFOC_CMD_DEFAULTS:
        if (c.state == DMFOC_ST_RUN) return DMFOC_E_STATE;
        persist_load_defaults();
        return DMFOC_OK;
    default:
        return DMFOC_E_ARG;
    }
}

void FocSim::persist_save() { saved_ = c_; }

void FocSim::persist_load_defaults()
{
    Ctrl rt = c_;
    c_ = defaults_;
    // keep runtime state and the calibration of this board
    c_.mode = rt.mode; c_.state = rt.state; c_.faults = rt.faults;
    c_.target_current = rt.target_current; c_.target_speed = rt.target_speed; c_.target_pos = rt.target_pos;
    c_.last_mech = rt.last_mech; c_.pos_acc = rt.pos_acc; c_.speed_f = rt.speed_f; c_.first = rt.first;
    c_.enc_offset = rt.enc_offset; c_.enc_dir = rt.enc_dir; c_.calibrated = rt.calibrated;
}
