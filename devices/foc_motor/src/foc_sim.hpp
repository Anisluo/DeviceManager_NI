// foc_sim.hpp — a simulated FOC motor controller board: PMSM + inverter + encoder model,
// a firmware-style FOC controller, and the real firmware protocol (dmfoc_proto.c) on top.
// The host talks to it through the same byte stream it would send to the UART.
#pragma once

#include "transport.hpp"

extern "C" {
#include "../firmware/dmfoc_proto.h"
}

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

class FocSim : public Transport {
public:
    explicit FocSim(unsigned seed = 1);
    ~FocSim() override;

    // Transport
    void write(const uint8_t* data, size_t n) override;
    size_t read(uint8_t* buf, size_t max, int timeout_ms) override;
    std::string name() const override { return "仿真控制板"; }

    // Simulation-only knobs (not part of the firmware protocol)
    void set_load(float torque_nm);
    void set_vbus(float volts);
    float load() const { return load_tl_; }

private:
    // -------- "firmware": controller state, parameters, channels
    struct Ctrl {
        // parameters (exposed through dmfoc_param_t)
        float cur_kp = 2.5f, cur_ki = 2200.0f;            // V/A, V/(A·s)  ~1 kHz bandwidth
        int32_t decouple = 1;
        float spd_kp = 0.010f, spd_ki = 0.30f;            // A/rpm, A/(rpm·s)
        float spd_lpf_hz = 300.0f;
        float pos_kp = 25.0f;                             // rpm/deg
        float i_max = 10.0f, speed_max = 4000.0f, accel = 20000.0f;   // A, rpm, rpm/s
        int32_t pole_pairs = 7;
        float rs = 0.35f, ld = 0.40e-3f, lq = 0.45e-3f, flux = 0.0050f;  // model values for decoupling
        float enc_offset = 0.0f;                          // electrical offset [rad]
        int32_t enc_dir = 0;                              // 0 normal, 1 reversed
        int32_t calibrated = 0;
        float cal_current = 4.0f;
        float vf_freq = 20.0f, vf_volt = 2.0f;            // Hz (electrical), V
        float oc_limit = 20.0f, ov_limit = 32.0f, uv_limit = 12.0f, ot_limit = 120.0f;
        float rs_est = 0.0f, ld_est = 0.0f;               // identification results
        // runtime
        int32_t mode = DMFOC_MODE_IDLE;
        uint8_t state = DMFOC_ST_IDLE;
        uint32_t faults = 0;
        float target_current = 0.0f, target_speed = 0.0f, target_pos = 0.0f;
        float id_i = 0.0f, iq_i = 0.0f, spd_i = 0.0f;     // integrators
        float speed_ramp = 0.0f, theta_ol = 0.0f;
        float last_mech = 0.0f, pos_acc = 0.0f, speed_f = 0.0f;
        bool  first = true;
        // routine state (calibration / identification)
        int   phase = 0;
        float t_phase = 0.0f, acc_a = 0.0f, acc_b = 0.0f;
        int   n_acc = 0;
        float cal_a = 0.0f;
        float u1 = 0.0f, i1 = 0.0f;                       // identification: first operating point
    } c_;

    // channels (sampled by STATUS and the scope)
    struct Ch {
        float id, iq, id_ref, iq_ref, ud, uq, speed, speed_ref, pos, pos_ref, theta_e, ia, ib, ic, vbus, temp, torque;
    } ch_{};

    // -------- plant
    struct Plant {
        float id = 0, iq = 0, wm = 0, thm = 0;            // true dq currents, mech speed/angle
        float rs = 0.35f, ld = 0.40e-3f, lq = 0.45e-3f, flux = 0.0050f, j = 3.0e-5f, b = 1.0e-5f;
        int   pp = 7;
        float vbus = 24.0f, temp = 25.0f;
        float enc_mech_offset = 0.0f;                     // mounting offset of the encoder [rad]
        bool  enabled = false;                            // inverter switching
    } m_;

    void step();                // one control period
    void plant(float ua, float ub, float dt);
    float encoder_mech() const;
    void enable_outputs(bool on);
    int8_t command(uint8_t cmd, float arg);
    void build_tables();
    void persist_save();
    void persist_load_defaults();

    static constexpr float TS = 50e-6f;                  // 20 kHz control loop
    dmfoc_t proto_{};
    std::vector<dmfoc_param_t> params_;
    std::vector<dmfoc_channel_t> channels_;
    std::vector<float> scope_buf_;
    Ctrl defaults_, saved_;
    float load_tl_ = 0.0f;
    std::mt19937 rng_;
    std::normal_distribution<float> noise_{0.0f, 0.02f};

    std::mutex mu_;                                       // protects everything above
    std::deque<uint8_t> out_;
    std::condition_variable out_cv_;
    std::atomic<bool> run_{true};
    std::thread thread_;
};
