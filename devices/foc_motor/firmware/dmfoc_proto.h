/*
 * dmfoc_proto.h — DeviceManager FOC debug protocol, firmware side (portable C99, no malloc).
 *
 * The MCU describes itself: parameters (name, unit, group, type, range) and
 * waveform channels are listed by the host at connect time, so adding a
 * parameter here makes it appear in the host UI without touching the plugin.
 *
 * Frame (both directions):
 *   A5 5A | TYPE | SEQ | LEN (u16 LE) | PAYLOAD[LEN] | CRC16 (u16 LE)
 *   CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over TYPE..PAYLOAD.
 *   Response TYPE = request TYPE | 0x80, same SEQ; errors: TYPE 0xFF, payload {req_type, code}.
 *
 * Integration (see dmfoc_port_example.c):
 *   static dmfoc_t g_dbg;
 *   dmfoc_init(&g_dbg, &cfg);                 // once
 *   dmfoc_rx(&g_dbg, bytes, n);               // from the UART RX path (main loop / RX ISR)
 *   dmfoc_scope_sample(&g_dbg);               // at the end of every FOC interrupt
 */
#ifndef DMFOC_PROTO_H
#define DMFOC_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMFOC_PROTO_VERSION   1
#define DMFOC_MAX_PAYLOAD     1024
#define DMFOC_SCOPE_MAX_CH    8

/* message types (requests; responses are | 0x80) */
enum {
    DMFOC_PING          = 0x01,  /* -> ver u8, n_params u16, n_channels u8, scope_capacity u16, rate f32, name str */
    DMFOC_PARAM_INFO    = 0x02,  /* id u16 -> id u16, type u8, flags u8, min f32, max f32, name\0 unit\0 group\0 enums\0 */
    DMFOC_PARAM_READ    = 0x03,  /* first u16, count u8 -> first u16, count u8, value[count] (4 bytes each) */
    DMFOC_PARAM_WRITE   = 0x04,  /* id u16, value(4) -> id u16, status u8, value(4) actually applied */
    DMFOC_CHANNEL_INFO  = 0x05,  /* id u8 -> id u8, name\0 unit\0 */
    DMFOC_COMMAND       = 0x06,  /* cmd u8, arg f32 -> cmd u8, result i8 */
    DMFOC_STATUS        = 0x07,  /* -> state u8, mode u8, faults u32, n u8, value[n] f32 (all channels) */
    DMFOC_SCOPE_CONFIG  = 0x08,  /* nch u8, ch[nch] u8, divider u16, length u16, trig_ch u8, edge u8, level f32, pre u16 */
    DMFOC_SCOPE_ARM     = 0x09,  /* mode u8 (0 = wait for trigger, 1 = trigger now) -> status u8 */
    DMFOC_SCOPE_STATUS  = 0x0A,  /* -> state u8, length u16, nch u8, pre u16, dt f32 */
    DMFOC_SCOPE_READ    = 0x0B,  /* offset u16, count u8 -> offset u16, count u8, f32[count * nch] (sample-major) */
    DMFOC_ERROR         = 0xFF
};

/* error codes (payload of DMFOC_ERROR, and PARAM_WRITE / SCOPE status) */
enum { DMFOC_OK = 0, DMFOC_E_CRC = 1, DMFOC_E_TYPE = 2, DMFOC_E_ARG = 3, DMFOC_E_RANGE = 4,
       DMFOC_E_READONLY = 5, DMFOC_E_STATE = 6, DMFOC_E_BUSY = 7 };

/* commands */
enum { DMFOC_CMD_ENABLE = 1, DMFOC_CMD_DISABLE = 2, DMFOC_CMD_CLEAR_FAULT = 3, DMFOC_CMD_SET_MODE = 4,
       DMFOC_CMD_SET_TARGET = 5, DMFOC_CMD_CALIBRATE = 6, DMFOC_CMD_IDENTIFY = 7, DMFOC_CMD_SAVE = 8,
       DMFOC_CMD_DEFAULTS = 9, DMFOC_CMD_ESTOP = 10 };

/* states / modes / faults (conventions shared with the host UI) */
enum { DMFOC_ST_IDLE = 0, DMFOC_ST_RUN = 1, DMFOC_ST_FAULT = 2, DMFOC_ST_CALIBRATE = 3, DMFOC_ST_IDENTIFY = 4 };
enum { DMFOC_MODE_IDLE = 0, DMFOC_MODE_VF = 1, DMFOC_MODE_CURRENT = 2, DMFOC_MODE_SPEED = 3, DMFOC_MODE_POSITION = 4 };
#define DMFOC_F_OVERCURRENT   (1u << 0)
#define DMFOC_F_OVERVOLTAGE   (1u << 1)
#define DMFOC_F_UNDERVOLTAGE  (1u << 2)
#define DMFOC_F_OVERTEMP      (1u << 3)
#define DMFOC_F_ENCODER       (1u << 4)
#define DMFOC_F_NOT_CALIBRATED (1u << 16)   /* warning bits from 16 up: not a fault, shown as a hint */

/* parameter description */
typedef enum { DMFOC_F32 = 0, DMFOC_I32 = 1, DMFOC_BOOL = 2, DMFOC_ENUM = 3 } dmfoc_type_t;
#define DMFOC_RO       0x01u   /* read-only (measured / identified values) */
#define DMFOC_PERSIST  0x02u   /* stored by DMFOC_CMD_SAVE */

typedef struct {
    const char* name;          /* short identifier, e.g. "cur_kp" */
    const char* unit;          /* "V/A", "A", "rpm" ... */
    const char* group;         /* UI grouping, e.g. "电流环" (UTF-8) */
    uint8_t     type;          /* dmfoc_type_t; DMFOC_I32/BOOL/ENUM point to int32_t, DMFOC_F32 to float */
    uint8_t     flags;
    float       min, max;
    void*       ptr;
    const char* enums;         /* DMFOC_ENUM: "IDLE|VF|CURRENT" (index = value) */
} dmfoc_param_t;

typedef struct {
    const char*           name;  /* e.g. "iq" */
    const char*           unit;  /* "A" */
    const volatile float* ptr;   /* sampled by dmfoc_scope_sample() and STATUS */
} dmfoc_channel_t;

typedef struct {
    const char*            fw_name;          /* shown by the host */
    const dmfoc_param_t*   params;
    uint16_t               n_params;
    const dmfoc_channel_t* channels;
    uint8_t                n_channels;
    float                  sample_rate_hz;   /* rate of dmfoc_scope_sample() calls */
    float*                 scope_buf;        /* user-provided RAM for captures */
    uint16_t               scope_capacity;   /* floats in scope_buf */
    /* callbacks */
    void   (*tx)(const uint8_t* data, uint16_t len, void* user);            /* send bytes to the UART */
    int8_t (*command)(uint8_t cmd, float arg, void* user);                  /* returns a DMFOC_* code */
    void   (*param_changed)(uint16_t id, void* user);                       /* optional */
    void   (*state)(uint8_t* state, uint8_t* mode, uint32_t* faults, void* user);
    void*  user;
} dmfoc_config_t;

typedef struct {
    dmfoc_config_t cfg;
    /* receiver */
    uint8_t  rx[DMFOC_MAX_PAYLOAD + 8];
    uint16_t rx_n, rx_need;
    /* transmit scratch */
    uint8_t  tx[DMFOC_MAX_PAYLOAD + 8];
    /* scope */
    volatile uint8_t  sc_state;              /* 0 idle, 1 armed, 2 triggered, 3 done */
    uint8_t  sc_nch, sc_ch[DMFOC_SCOPE_MAX_CH], sc_trig_ch, sc_edge;
    uint16_t sc_div, sc_div_cnt, sc_len, sc_pre, sc_head, sc_filled, sc_post, sc_start;
    float    sc_level, sc_prev;
    uint8_t  sc_force;
} dmfoc_t;

void dmfoc_init(dmfoc_t* ctx, const dmfoc_config_t* cfg);
void dmfoc_rx(dmfoc_t* ctx, const uint8_t* data, uint16_t len);
void dmfoc_scope_sample(dmfoc_t* ctx);
uint16_t dmfoc_crc16(const uint8_t* data, uint16_t len, uint16_t crc);

#ifdef __cplusplus
}
#endif
#endif /* DMFOC_PROTO_H */
