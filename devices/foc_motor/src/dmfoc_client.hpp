// dmfoc_client.hpp — host side of the DMFOC protocol (request / response over a Transport).
#pragma once

#include "transport.hpp"

extern "C" {
#include "../firmware/dmfoc_proto.h"
}

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

class ProtoError : public std::runtime_error {
public:
    ProtoError(const std::string& m, int code = 0) : std::runtime_error(m), code(code) {}
    int code;
};

// Little-endian payload builder / reader
struct Buf {
    std::vector<uint8_t> b;
    Buf& u8(uint8_t v) { b.push_back(v); return *this; }
    Buf& u16(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); return *this; }
    Buf& u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Buf& f32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u32(u); }
};

struct Reader {
    const std::vector<uint8_t>& b;
    size_t p = 0;
    void need(size_t n) const { if (p + n > b.size()) throw ProtoError("响应太短"); }
    uint8_t u8() { need(1); return b[p++]; }
    uint16_t u16() { need(2); uint16_t v = uint16_t(b[p] | (b[p + 1] << 8)); p += 2; return v; }
    uint32_t u32() { need(4); uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(b[p + i]) << (8 * i); p += 4; return v; }
    int32_t i32() { return int32_t(u32()); }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    std::string str() { std::string s; while (p < b.size() && b[p]) s += char(b[p++]); if (p < b.size()) ++p; return s; }
};

class DmfocClient {
public:
    explicit DmfocClient(Transport* t) : t_(t) {}

    std::vector<uint8_t> request(uint8_t type, const std::vector<uint8_t>& payload = {}, int timeout_ms = 300, int retries = 2)
    {
        std::string last;
        for (int attempt = 0; attempt <= retries; ++attempt) {
            uint8_t seq = ++seq_;
            std::vector<uint8_t> f{0xA5, 0x5A, type, seq, uint8_t(payload.size()), uint8_t(payload.size() >> 8)};
            f.insert(f.end(), payload.begin(), payload.end());
            uint16_t crc = dmfoc_crc16(f.data() + 2, uint16_t(f.size() - 2), 0xFFFF);
            f.push_back(uint8_t(crc));
            f.push_back(uint8_t(crc >> 8));
            t_->flush_input();
            rx_.clear();
            t_->write(f.data(), f.size());
            try {
                return receive(type, seq, timeout_ms);
            } catch (const ProtoError& e) {
                if (e.code > 0) throw;                      // the MCU answered with an error: don't retry
                last = e.what();
                ++stats_retries;
            }
        }
        ++stats_errors;
        throw ProtoError(last + "（控制板无响应：检查串口、波特率和固件）");
    }

    unsigned stats_retries = 0, stats_errors = 0;

private:
    std::vector<uint8_t> receive(uint8_t type, uint8_t seq, int timeout_ms)
    {
        uint8_t tmp[2048];
        int waited = 0;
        while (true) {
            // try to parse a frame from rx_
            size_t i = 0;
            while (i + 1 < rx_.size() && !(rx_[i] == 0xA5 && rx_[i + 1] == 0x5A)) ++i;
            if (i) rx_.erase(rx_.begin(), rx_.begin() + long(i));
            if (rx_.size() >= 6) {
                uint16_t len = uint16_t(rx_[4] | (rx_[5] << 8));
                if (len > DMFOC_MAX_PAYLOAD) { rx_.erase(rx_.begin()); continue; }
                if (rx_.size() >= size_t(len) + 8) {
                    uint16_t crc = uint16_t(rx_[6 + len] | (rx_[7 + len] << 8));
                    bool ok = crc == dmfoc_crc16(rx_.data() + 2, uint16_t(len + 4), 0xFFFF);
                    uint8_t rtype = rx_[2], rseq = rx_[3];
                    std::vector<uint8_t> payload(rx_.begin() + 6, rx_.begin() + 6 + len);
                    rx_.erase(rx_.begin(), rx_.begin() + long(len) + 8);
                    if (!ok || rseq != seq) continue;          // stale or damaged frame
                    if (rtype == DMFOC_ERROR) {
                        int code = payload.size() >= 2 ? payload[1] : 255;
                        throw ProtoError(error_text(code), code);
                    }
                    if (rtype == (type | 0x80)) return payload;
                    continue;
                }
            }
            if (waited >= timeout_ms) throw ProtoError("响应超时");
            size_t n = t_->read(tmp, sizeof tmp, 20);
            if (n) rx_.insert(rx_.end(), tmp, tmp + n);
            else waited += 20;
        }
    }

public:
    static std::string error_text(int code)
    {
        switch (code) {
        case DMFOC_E_CRC: return "控制板收到的帧 CRC 错误";
        case DMFOC_E_TYPE: return "控制板不支持该命令";
        case DMFOC_E_ARG: return "参数错误";
        case DMFOC_E_RANGE: return "数值超出范围，已限幅";
        case DMFOC_E_READONLY: return "只读参数";
        case DMFOC_E_STATE: return "当前状态不允许该操作";
        case DMFOC_E_BUSY: return "控制板忙";
        default: return "错误码 " + std::to_string(code);
        }
    }

private:
    Transport* t_;
    uint8_t seq_ = 0;
    std::vector<uint8_t> rx_;
};
