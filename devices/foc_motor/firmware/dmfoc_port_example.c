/*
 * dmfoc_port_example.c — how to hook dmfoc_proto into an existing FOC firmware.
 *
 * Copy dmfoc_proto.h / dmfoc_proto.c into the project, then adapt this file:
 *   - uart_write()        : blocking or DMA transmit on the USB-UART
 *   - UART RX             : feed received bytes to dmfoc_rx() from the main loop
 *                           (a ring buffer filled by the RX ISR / DMA keeps the ISR short)
 *   - foc_isr()           : your current-loop interrupt; call dmfoc_scope_sample() at its end
 *   - g_foc / foc_*()     : your controller state and actions
 *
 * The parameter names below are what the host's "校准 / 辨识" and "整定" pages use
 * (enc_offset, enc_dir, calibrated, rs, ld, lq, rs_est, ld_est, accel). Everything else is
 * free: add a row to PARAMS and it appears on the 参数 page, add a row to CHANNELS and it can
 * be captured by the scope.
 */
#include "dmfoc_proto.h"

#include <string.h>

/* ------------------------------------------------------------------ your firmware */

typedef struct {
    /* tunables */
    float cur_kp, cur_ki, spd_kp, spd_ki, accel, pos_kp;
    float i_max, speed_max, oc_limit;
    int32_t pole_pairs;
    float rs, ld, lq, flux;
    float enc_offset;
    int32_t enc_dir, calibrated;
    float rs_est, ld_est;
    /* live values (written by the ISR, read by the scope) */
    volatile float id, iq, id_ref, iq_ref, ud, uq, speed, speed_ref, pos, pos_ref, ia, ib, ic, vbus, temp;
    /* state */
    uint8_t state, mode;
    uint32_t faults;
    float target;
} foc_t;

static foc_t g_foc;

extern void uart_write(const uint8_t* data, uint16_t len);   /* your HAL */
extern int  uart_read(uint8_t* data, uint16_t max);           /* non-blocking, returns bytes read */
extern void foc_enable(int on);
extern void foc_start_calibration(void);                      /* sets state = CALIBRATE, back to IDLE when done */
extern void foc_start_identification(void);                   /* fills rs_est / ld_est */
extern int  flash_save(const void* data, uint32_t len);
extern void foc_load_defaults(foc_t* f);

/* ------------------------------------------------------------------ tables */

static const dmfoc_param_t PARAMS[] = {
    /* name        unit         group        type        flags          min    max     ptr */
    {"cur_kp",     "V/A",       "电流环",    DMFOC_F32,  DMFOC_PERSIST, 0,     50,     &g_foc.cur_kp, 0},
    {"cur_ki",     "V/(A·s)",   "电流环",    DMFOC_F32,  DMFOC_PERSIST, 0,     50000,  &g_foc.cur_ki, 0},
    {"spd_kp",     "A/rpm",     "速度环",    DMFOC_F32,  DMFOC_PERSIST, 0,     1,      &g_foc.spd_kp, 0},
    {"spd_ki",     "A/(rpm·s)", "速度环",    DMFOC_F32,  DMFOC_PERSIST, 0,     20,     &g_foc.spd_ki, 0},
    {"accel",      "rpm/s",     "速度环",    DMFOC_F32,  DMFOC_PERSIST, 10,    200000, &g_foc.accel, 0},
    {"pos_kp",     "rpm/°",     "位置环",    DMFOC_F32,  DMFOC_PERSIST, 0,     100,    &g_foc.pos_kp, 0},
    {"i_max",      "A",         "限制",      DMFOC_F32,  DMFOC_PERSIST, 0.1f,  15,     &g_foc.i_max, 0},
    {"speed_max",  "rpm",       "限制",      DMFOC_F32,  DMFOC_PERSIST, 10,    6000,   &g_foc.speed_max, 0},
    {"oc_limit",   "A",         "保护",      DMFOC_F32,  DMFOC_PERSIST, 1,     30,     &g_foc.oc_limit, 0},
    {"pole_pairs", "",          "电机",      DMFOC_I32,  DMFOC_PERSIST, 1,     50,     &g_foc.pole_pairs, 0},
    {"rs",         "Ω",         "电机",      DMFOC_F32,  DMFOC_PERSIST, 0.001f,100,    &g_foc.rs, 0},
    {"ld",         "H",         "电机",      DMFOC_F32,  DMFOC_PERSIST, 1e-6f, 1,      &g_foc.ld, 0},
    {"lq",         "H",         "电机",      DMFOC_F32,  DMFOC_PERSIST, 1e-6f, 1,      &g_foc.lq, 0},
    {"flux",       "Wb",        "电机",      DMFOC_F32,  DMFOC_PERSIST, 0,     1,      &g_foc.flux, 0},
    {"enc_offset", "rad",       "编码器",    DMFOC_F32,  DMFOC_PERSIST, 0,     6.2832f,&g_foc.enc_offset, 0},
    {"enc_dir",    "",          "编码器",    DMFOC_ENUM, DMFOC_PERSIST, 0,     1,      &g_foc.enc_dir, "NORMAL|REVERSED"},
    {"calibrated", "",          "编码器",    DMFOC_BOOL, DMFOC_RO,      0,     1,      &g_foc.calibrated, 0},
    {"rs_est",     "Ω",         "辨识结果",  DMFOC_F32,  DMFOC_RO,      0,     0,      &g_foc.rs_est, 0},
    {"ld_est",     "H",         "辨识结果",  DMFOC_F32,  DMFOC_RO,      0,     0,      &g_foc.ld_est, 0},
};

static const dmfoc_channel_t CHANNELS[] = {
    {"id", "A", &g_foc.id},           {"iq", "A", &g_foc.iq},
    {"id_ref", "A", &g_foc.id_ref},   {"iq_ref", "A", &g_foc.iq_ref},
    {"ud", "V", &g_foc.ud},           {"uq", "V", &g_foc.uq},
    {"speed", "rpm", &g_foc.speed},   {"speed_ref", "rpm", &g_foc.speed_ref},
    {"pos", "°", &g_foc.pos},         {"pos_ref", "°", &g_foc.pos_ref},
    {"ia", "A", &g_foc.ia},           {"ib", "A", &g_foc.ib},          {"ic", "A", &g_foc.ic},
    {"vbus", "V", &g_foc.vbus},       {"temp", "°C", &g_foc.temp},
};

/* 4 channels x 1000 points = 16 KB. Size it to the RAM you can spare. */
static float g_scope_buf[4000];

static dmfoc_t g_dbg;

/* ------------------------------------------------------------------ callbacks */

static void tx(const uint8_t* data, uint16_t len, void* user)
{
    (void)user;
    uart_write(data, len);
}

static int8_t command(uint8_t cmd, float arg, void* user)
{
    (void)user;
    switch (cmd) {
    case DMFOC_CMD_ENABLE:
        if (g_foc.faults & 0xFFFFu) return DMFOC_E_STATE;
        if (!g_foc.calibrated && g_foc.mode >= DMFOC_MODE_CURRENT) return DMFOC_E_STATE;
        foc_enable(1);
        g_foc.state = DMFOC_ST_RUN;
        return DMFOC_OK;
    case DMFOC_CMD_DISABLE:
    case DMFOC_CMD_ESTOP:
        foc_enable(0);
        g_foc.state = DMFOC_ST_IDLE;
        return DMFOC_OK;
    case DMFOC_CMD_CLEAR_FAULT:
        g_foc.faults &= ~0xFFFFu;
        if (g_foc.state == DMFOC_ST_FAULT) g_foc.state = DMFOC_ST_IDLE;
        return DMFOC_OK;
    case DMFOC_CMD_SET_MODE:
        if (arg < 0 || arg > DMFOC_MODE_POSITION) return DMFOC_E_ARG;
        g_foc.mode = (uint8_t)arg;
        return DMFOC_OK;
    case DMFOC_CMD_SET_TARGET:
        g_foc.target = arg;                    /* A, rpm or ° depending on mode; ISR reads it */
        return DMFOC_OK;
    case DMFOC_CMD_CALIBRATE:
        if (g_foc.state == DMFOC_ST_RUN) return DMFOC_E_STATE;
        foc_start_calibration();
        return DMFOC_OK;
    case DMFOC_CMD_IDENTIFY:
        if (g_foc.state == DMFOC_ST_RUN) return DMFOC_E_STATE;
        foc_start_identification();
        return DMFOC_OK;
    case DMFOC_CMD_SAVE:
        return flash_save(&g_foc, sizeof g_foc) == 0 ? DMFOC_OK : DMFOC_E_BUSY;
    case DMFOC_CMD_DEFAULTS:
        foc_load_defaults(&g_foc);
        return DMFOC_OK;
    default:
        return DMFOC_E_ARG;
    }
}

static void state(uint8_t* st, uint8_t* mode, uint32_t* faults, void* user)
{
    (void)user;
    *st = g_foc.state;
    *mode = g_foc.mode;
    *faults = g_foc.faults | (g_foc.calibrated ? 0 : DMFOC_F_NOT_CALIBRATED);
}

/* ------------------------------------------------------------------ glue */

void debug_init(void)
{
    dmfoc_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.fw_name = "MyFOC v1.0";
    cfg.params = PARAMS;
    cfg.n_params = sizeof PARAMS / sizeof PARAMS[0];
    cfg.channels = CHANNELS;
    cfg.n_channels = sizeof CHANNELS / sizeof CHANNELS[0];
    cfg.sample_rate_hz = 20000.0f;             /* = FOC interrupt rate */
    cfg.scope_buf = g_scope_buf;
    cfg.scope_capacity = sizeof g_scope_buf / sizeof g_scope_buf[0];
    cfg.tx = tx;
    cfg.command = command;
    cfg.state = state;
    dmfoc_init(&g_dbg, &cfg);
}

/* main loop */
void debug_poll(void)
{
    uint8_t buf[64];
    int n;
    while ((n = uart_read(buf, sizeof buf)) > 0) dmfoc_rx(&g_dbg, buf, (uint16_t)n);
}

/* current-loop interrupt (20 kHz) */
void foc_isr(void)
{
    /* ... ADC, Clarke/Park, PI, SVPWM; update g_foc.id / iq / speed ... */
    dmfoc_scope_sample(&g_dbg);                /* a few hundred cycles; no UART work here */
}
