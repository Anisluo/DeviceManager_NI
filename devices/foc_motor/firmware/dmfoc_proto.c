/* dmfoc_proto.c — DeviceManager FOC debug protocol, firmware side. See dmfoc_proto.h. */
#include "dmfoc_proto.h"

#include <string.h>

/* ---------------------------------------------------------------- helpers */

uint16_t dmfoc_crc16(const uint8_t* d, uint16_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*d++) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static float rdf(const uint8_t* p) { float f; memcpy(&f, p, 4); return f; }
static void wrf(uint8_t* p, float f) { memcpy(p, &f, 4); }

static uint16_t put_str(uint8_t* p, uint16_t room, const char* s)
{
    uint16_t n = 0;
    if (s) while (s[n] && n + 1 < room) { p[n] = (uint8_t)s[n]; ++n; }
    p[n] = 0;
    return (uint16_t)(n + 1);
}

/* send TYPE|SEQ|payload already placed at ctx->tx + 6 */
static void send(dmfoc_t* ctx, uint8_t type, uint8_t seq, uint16_t len)
{
    uint8_t* f = ctx->tx;
    f[0] = 0xA5; f[1] = 0x5A; f[2] = type; f[3] = seq;
    wr16(f + 4, len);
    wr16(f + 6 + len, dmfoc_crc16(f + 2, (uint16_t)(len + 4), 0xFFFF));
    ctx->cfg.tx(f, (uint16_t)(len + 8), ctx->cfg.user);
}

static void send_error(dmfoc_t* ctx, uint8_t req, uint8_t seq, uint8_t code)
{
    ctx->tx[6] = req;
    ctx->tx[7] = code;
    send(ctx, DMFOC_ERROR, seq, 2);
}

static int enum_count(const char* e)
{
    int n = e && *e ? 1 : 0;
    for (; e && *e; ++e) if (*e == '|') ++n;
    return n;
}

static void param_get(const dmfoc_param_t* p, uint8_t* out)
{
    if (p->type == DMFOC_F32) wrf(out, *(const float*)p->ptr);
    else wr32(out, (uint32_t)*(const int32_t*)p->ptr);
}

/* ---------------------------------------------------------------- scope */

static int scope_config(dmfoc_t* c, const uint8_t* p, uint16_t len)
{
    if (len < 1) return DMFOC_E_ARG;
    uint8_t nch = p[0];
    if (nch == 0 || nch > DMFOC_SCOPE_MAX_CH || len < (uint16_t)(1 + nch + 2 + 2 + 1 + 1 + 4 + 2)) return DMFOC_E_ARG;
    for (uint8_t i = 0; i < nch; ++i)
        if (p[1 + i] >= c->cfg.n_channels) return DMFOC_E_ARG;
    const uint8_t* q = p + 1 + nch;
    uint16_t div = rd16(q), length = rd16(q + 2);
    uint8_t trig = q[4], edge = q[5];
    float level = rdf(q + 6);
    uint16_t pre = rd16(q + 10);
    if (div == 0) div = 1;
    if (length < 2) return DMFOC_E_ARG;
    if ((uint32_t)length * nch > c->cfg.scope_capacity) length = (uint16_t)(c->cfg.scope_capacity / nch);
    if (pre >= length) pre = (uint16_t)(length - 1);
    if (trig != 0xFF && trig >= c->cfg.n_channels) return DMFOC_E_ARG;
    c->sc_state = 0;
    c->sc_nch = nch;
    memcpy(c->sc_ch, p + 1, nch);
    c->sc_div = div; c->sc_len = length; c->sc_trig_ch = trig; c->sc_edge = edge;
    c->sc_level = level; c->sc_pre = pre;
    return DMFOC_OK;
}

static void scope_arm(dmfoc_t* c, uint8_t force)
{
    c->sc_state = 0;
    c->sc_head = 0; c->sc_filled = 0; c->sc_div_cnt = 0;
    c->sc_force = (uint8_t)(force || c->sc_trig_ch == 0xFF);
    c->sc_prev = 0.0f;
    if (c->sc_len) c->sc_state = 1;
}

void dmfoc_scope_sample(dmfoc_t* c)
{
    uint8_t st = c->sc_state;
    if (st != 1 && st != 2) return;
    if (++c->sc_div_cnt < c->sc_div) return;
    c->sc_div_cnt = 0;

    float* row = c->cfg.scope_buf + (uint32_t)c->sc_head * c->sc_nch;
    for (uint8_t i = 0; i < c->sc_nch; ++i) row[i] = *c->cfg.channels[c->sc_ch[i]].ptr;
    c->sc_head = (uint16_t)((c->sc_head + 1) % c->sc_len);

    if (st == 1) {
        if (c->sc_filled < c->sc_len) ++c->sc_filled;
        float v = c->sc_trig_ch == 0xFF ? 0.0f : *c->cfg.channels[c->sc_trig_ch].ptr;
        int hit = c->sc_force;
        if (!hit && c->sc_filled > c->sc_pre && c->sc_filled > 1) {
            int up = c->sc_prev < c->sc_level && v >= c->sc_level;
            int down = c->sc_prev > c->sc_level && v <= c->sc_level;
            hit = c->sc_edge == 0 ? up : c->sc_edge == 1 ? down : (up || down);
        }
        c->sc_prev = v;
        if (hit && c->sc_filled > c->sc_pre) {
            c->sc_post = (uint16_t)(c->sc_len - c->sc_pre - 1);
            c->sc_state = 2;
            if (c->sc_post == 0) { c->sc_start = c->sc_head; c->sc_state = 3; }
        }
    } else if (--c->sc_post == 0) {
        c->sc_start = c->sc_head;          /* ring now holds pre + trigger + post, oldest at head */
        c->sc_state = 3;
    }
}

/* ---------------------------------------------------------------- dispatch */

static void handle(dmfoc_t* c, uint8_t type, uint8_t seq, const uint8_t* p, uint16_t len)
{
    uint8_t* o = c->tx + 6;
    const dmfoc_config_t* g = &c->cfg;
    uint8_t rt = (uint8_t)(type | 0x80);

    switch (type) {
    case DMFOC_PING: {
        o[0] = DMFOC_PROTO_VERSION;
        wr16(o + 1, g->n_params);
        o[3] = g->n_channels;
        wr16(o + 4, g->scope_capacity);
        wrf(o + 6, g->sample_rate_hz);
        uint16_t n = put_str(o + 10, 64, g->fw_name);
        send(c, rt, seq, (uint16_t)(10 + n));
        return;
    }
    case DMFOC_PARAM_INFO: {
        if (len < 2) break;
        uint16_t id = rd16(p);
        if (id >= g->n_params) { send_error(c, type, seq, DMFOC_E_ARG); return; }
        const dmfoc_param_t* q = &g->params[id];
        wr16(o, id); o[2] = q->type; o[3] = q->flags; wrf(o + 4, q->min); wrf(o + 8, q->max);
        uint16_t n = 12;
        n = (uint16_t)(n + put_str(o + n, 48, q->name));
        n = (uint16_t)(n + put_str(o + n, 24, q->unit));
        n = (uint16_t)(n + put_str(o + n, 48, q->group));
        n = (uint16_t)(n + put_str(o + n, 400, q->enums));
        send(c, rt, seq, n);
        return;
    }
    case DMFOC_PARAM_READ: {
        if (len < 3) break;
        uint16_t first = rd16(p);
        uint8_t count = p[2];
        if (count > 64 || first + count > g->n_params) { send_error(c, type, seq, DMFOC_E_ARG); return; }
        wr16(o, first); o[2] = count;
        for (uint8_t i = 0; i < count; ++i) param_get(&g->params[first + i], o + 3 + 4 * i);
        send(c, rt, seq, (uint16_t)(3 + 4 * count));
        return;
    }
    case DMFOC_PARAM_WRITE: {
        if (len < 6) break;
        uint16_t id = rd16(p);
        if (id >= g->n_params) { send_error(c, type, seq, DMFOC_E_ARG); return; }
        const dmfoc_param_t* q = &g->params[id];
        uint8_t st = DMFOC_OK;
        if (q->flags & DMFOC_RO) {
            st = DMFOC_E_READONLY;
        } else if (q->type == DMFOC_F32) {
            float v = rdf(p + 2);
            if (v != v) st = DMFOC_E_ARG;                               /* NaN */
            else {
                if (q->min < q->max && v < q->min) { v = q->min; st = DMFOC_E_RANGE; }
                if (q->min < q->max && v > q->max) { v = q->max; st = DMFOC_E_RANGE; }
                *(float*)q->ptr = v;
            }
        } else {
            int32_t v = (int32_t)(p[2] | (p[3] << 8) | (p[4] << 16) | ((uint32_t)p[5] << 24));
            int32_t lo = (int32_t)q->min, hi = (int32_t)q->max;
            if (q->type == DMFOC_BOOL) { lo = 0; hi = 1; }
            if (q->type == DMFOC_ENUM) { lo = 0; hi = enum_count(q->enums) - 1; }
            if (lo < hi && v < lo) { v = lo; st = DMFOC_E_RANGE; }
            if (lo < hi && v > hi) { v = hi; st = DMFOC_E_RANGE; }
            *(int32_t*)q->ptr = v;
        }
        if (st != DMFOC_E_READONLY && st != DMFOC_E_ARG && g->param_changed) g->param_changed(id, g->user);
        wr16(o, id); o[2] = st;
        param_get(q, o + 3);
        send(c, rt, seq, 7);
        return;
    }
    case DMFOC_CHANNEL_INFO: {
        if (len < 1) break;
        if (p[0] >= g->n_channels) { send_error(c, type, seq, DMFOC_E_ARG); return; }
        o[0] = p[0];
        uint16_t n = 1;
        n = (uint16_t)(n + put_str(o + n, 32, g->channels[p[0]].name));
        n = (uint16_t)(n + put_str(o + n, 16, g->channels[p[0]].unit));
        send(c, rt, seq, n);
        return;
    }
    case DMFOC_COMMAND: {
        if (len < 5) break;
        o[0] = p[0];
        o[1] = (uint8_t)(g->command ? g->command(p[0], rdf(p + 1), g->user) : DMFOC_E_TYPE);
        send(c, rt, seq, 2);
        return;
    }
    case DMFOC_STATUS: {
        uint8_t st = 0, mode = 0;
        uint32_t faults = 0;
        if (g->state) g->state(&st, &mode, &faults, g->user);
        o[0] = st; o[1] = mode; wr32(o + 2, faults);
        uint8_t n = g->n_channels > 64 ? 64 : g->n_channels;
        o[6] = n;
        for (uint8_t i = 0; i < n; ++i) wrf(o + 7 + 4 * i, *g->channels[i].ptr);
        send(c, rt, seq, (uint16_t)(7 + 4 * n));
        return;
    }
    case DMFOC_SCOPE_CONFIG: {
        int r = scope_config(c, p, len);
        o[0] = (uint8_t)r;
        wr16(o + 1, c->sc_len);
        send(c, rt, seq, 3);
        return;
    }
    case DMFOC_SCOPE_ARM: {
        if (len < 1) break;
        if (!c->sc_len) { o[0] = DMFOC_E_STATE; send(c, rt, seq, 1); return; }
        scope_arm(c, p[0]);
        o[0] = DMFOC_OK;
        send(c, rt, seq, 1);
        return;
    }
    case DMFOC_SCOPE_STATUS: {
        o[0] = c->sc_state;
        wr16(o + 1, c->sc_len);
        o[3] = c->sc_nch;
        wr16(o + 4, c->sc_pre);
        wrf(o + 6, g->sample_rate_hz > 0 ? (float)c->sc_div / g->sample_rate_hz : 0.0f);
        send(c, rt, seq, 10);
        return;
    }
    case DMFOC_SCOPE_READ: {
        if (len < 3) break;
        if (c->sc_state != 3) { send_error(c, type, seq, DMFOC_E_STATE); return; }
        uint16_t off = rd16(p);
        uint8_t count = p[2];
        uint16_t max_count = (uint16_t)((DMFOC_MAX_PAYLOAD - 3) / (4u * c->sc_nch));
        if (count > max_count) count = (uint8_t)max_count;
        if (off >= c->sc_len) { send_error(c, type, seq, DMFOC_E_ARG); return; }
        if (off + count > c->sc_len) count = (uint8_t)(c->sc_len - off);
        wr16(o, off); o[2] = count;
        uint8_t* w = o + 3;
        for (uint16_t s = 0; s < count; ++s) {
            uint16_t ring = (uint16_t)((c->sc_start + off + s) % c->sc_len);
            const float* row = g->scope_buf + (uint32_t)ring * c->sc_nch;
            for (uint8_t i = 0; i < c->sc_nch; ++i, w += 4) wrf(w, row[i]);
        }
        send(c, rt, seq, (uint16_t)(w - o));
        return;
    }
    default:
        send_error(c, type, seq, DMFOC_E_TYPE);
        return;
    }
    send_error(c, type, seq, DMFOC_E_ARG);   /* short payload */
}

/* ---------------------------------------------------------------- framing */

void dmfoc_init(dmfoc_t* ctx, const dmfoc_config_t* cfg)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->cfg = *cfg;
}

void dmfoc_rx(dmfoc_t* c, const uint8_t* data, uint16_t len)
{
    for (uint16_t k = 0; k < len; ++k) {
        uint8_t b = data[k];
        if (c->rx_n == 0) { if (b == 0xA5) c->rx[c->rx_n++] = b; continue; }
        if (c->rx_n == 1) { if (b == 0x5A) c->rx[c->rx_n++] = b; else c->rx_n = (b == 0xA5); continue; }
        c->rx[c->rx_n++] = b;
        if (c->rx_n == 6) {
            uint16_t plen = rd16(c->rx + 4);
            if (plen > DMFOC_MAX_PAYLOAD) { c->rx_n = 0; continue; }
            c->rx_need = (uint16_t)(plen + 8);
        }
        if (c->rx_n >= 6 && c->rx_n == c->rx_need) {
            uint16_t plen = rd16(c->rx + 4);
            uint16_t crc = rd16(c->rx + 6 + plen);
            if (crc == dmfoc_crc16(c->rx + 2, (uint16_t)(plen + 4), 0xFFFF))
                handle(c, c->rx[2], c->rx[3], c->rx + 6, plen);
            else
                send_error(c, c->rx[2], c->rx[3], DMFOC_E_CRC);
            c->rx_n = 0;
        }
    }
}
