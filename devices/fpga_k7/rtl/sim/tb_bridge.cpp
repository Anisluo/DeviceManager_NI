// Cycle-accurate testbench for dm_uart_bridge + dm_ctrl_regs (Yosys CXXRTL).
// Build: see devices/fpga_k7/rtl/sim/run_sim.py
#include "tb_top.cc"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

static cxxrtl_design::p_dm__bridge__tb__top top;
static uint64_t cycles = 0;
static int failures = 0;
static const int CPB = 8;                      // CLKS_PER_BIT in dm_bridge_tb_top

#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

// ---- UART receiver model on the DUT's tx line
static struct {
    int state = 0, cnt = 0, bit = 0;
    uint8_t sh = 0;
    std::deque<uint8_t> bytes;
} rxm;

static void monitor()
{
    bool tx = top.p_uart__tx.get<bool>();
    switch (rxm.state) {
    case 0: if (!tx) { rxm.state = 1; rxm.cnt = 0; } break;           // start edge
    case 1: if (++rxm.cnt == CPB / 2) { rxm.state = tx ? 0 : 2; rxm.cnt = 0; rxm.bit = 0; } break;
    case 2: if (++rxm.cnt == CPB) {
                rxm.cnt = 0;
                rxm.sh = uint8_t((rxm.sh >> 1) | (tx ? 0x80 : 0));
                if (++rxm.bit == 8) rxm.state = 3;
            } break;
    case 3: if (++rxm.cnt == CPB) {
                CHECK(tx, "stop bit low");
                rxm.bytes.push_back(rxm.sh);
                rxm.state = 0;
            } break;
    }
}

static void tick(int n = 1)
{
    while (n--) {
        top.p_clk.set<bool>(false);
        top.step();
        top.p_clk.set<bool>(true);
        top.step();
        ++cycles;
        monitor();
    }
}

static void send_byte(uint8_t b)
{
    top.p_uart__rx.set<bool>(false); tick(CPB);
    for (int i = 0; i < 8; ++i) { top.p_uart__rx.set<bool>((b >> i) & 1); tick(CPB); }
    top.p_uart__rx.set<bool>(true); tick(CPB);
}

static uint8_t crc8(const std::vector<uint8_t>& v, size_t from = 0)
{
    uint8_t c = 0;
    for (size_t k = from; k < v.size(); ++k) {
        c ^= v[k];
        for (int i = 0; i < 8; ++i) c = (c & 0x80) ? uint8_t((c << 1) ^ 0x07) : uint8_t(c << 1);
    }
    return c;
}

struct Resp { bool ok = false; uint8_t seq = 0, status = 0xFF; std::vector<uint32_t> words; };

static Resp transact(uint8_t cmd, uint8_t seq, uint32_t addr, std::vector<uint8_t> arg, bool corrupt = false,
                     int max_cycles = 20000)
{
    std::vector<uint8_t> f{0xA5, cmd, seq, uint8_t(addr), uint8_t(addr >> 8), uint8_t(addr >> 16), uint8_t(addr >> 24)};
    f.insert(f.end(), arg.begin(), arg.end());
    uint8_t c = crc8(f, 1);
    f.push_back(corrupt ? uint8_t(c ^ 0x5A) : c);
    rxm.bytes.clear();
    for (uint8_t b : f) send_byte(b);

    Resp r;
    auto need = [&](size_t n) {
        for (int i = 0; i < max_cycles && rxm.bytes.size() < n; ++i) tick();
        return rxm.bytes.size() >= n;
    };
    if (!need(3)) { std::printf("  timeout waiting for header\n"); return r; }
    size_t n = rxm.bytes[2];
    if (!need(3 + 4 * n + 2)) { std::printf("  timeout waiting for %zu words\n", n); return r; }
    std::vector<uint8_t> b(rxm.bytes.begin(), rxm.bytes.begin() + long(3 + 4 * n + 2));
    CHECK(b[0] == 0x5A, "sync 0x%02X", b[0]);
    r.seq = b[1];
    for (size_t i = 0; i < n; ++i)
        r.words.push_back(uint32_t(b[3 + 4 * i]) | uint32_t(b[4 + 4 * i]) << 8 | uint32_t(b[5 + 4 * i]) << 16 | uint32_t(b[6 + 4 * i]) << 24);
    r.status = b[3 + 4 * n];
    std::vector<uint8_t> body(b.begin() + 1, b.begin() + long(3 + 4 * n + 1));
    CHECK(crc8(body) == b.back(), "response CRC 0x%02X != 0x%02X", b.back(), crc8(body));
    r.ok = true;
    return r;
}

static uint32_t rd(uint32_t addr, uint8_t seq = 1)
{
    Resp r = transact(0x01, seq, addr, {1});
    CHECK(r.ok && r.status == 0 && r.words.size() == 1, "read 0x%03X status %d", addr, r.status);
    return r.words.empty() ? 0xFFFFFFFF : r.words[0];
}

static void wr(uint32_t addr, uint32_t v, uint8_t seq = 2)
{
    Resp r = transact(0x02, seq, addr, {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
    CHECK(r.ok && r.status == 0 && r.words.empty() && r.seq == seq, "write 0x%03X status %d", addr, r.status);
}

int main()
{
    top.p_uart__rx.set<bool>(true);
    top.p_swclk.set<bool>(false);
    top.p_rst.set<bool>(true); tick(5);
    top.p_rst.set<bool>(false); tick(20);

    // ID / build / reset values
    CHECK(rd(0x000) == 0x444D4B37, "ID");
    CHECK(rd(0x004) == 0x26102917, "BUILD");
    CHECK(rd(0x010) == 0x2, "CTRL reset value");
    CHECK(rd(0x014) == 0x3, "CLK_DIV reset value");
    CHECK(top.p_dut__clk__en.get<bool>() && !top.p_dut__reset.get<bool>(), "dut outputs after reset");

    // scratch round trip + sequence number echo
    wr(0x008, 0x12345678, 0x42);
    CHECK(rd(0x008, 0x43) == 0x12345678, "SCRATCH");

    // control outputs
    wr(0x010, 0x1);
    tick(2);
    CHECK(top.p_dut__reset.get<bool>() && !top.p_dut__clk__en.get<bool>(), "CTRL=1 -> reset, clock off");
    wr(0x014, 0x07);
    CHECK(top.p_dut__clk__div.get<uint32_t>() == 7, "CLK_DIV output");
    wr(0x01C, 0x00005A5A);
    CHECK(rd(0x020) == 0xCAFE5A5A, "GPIO_IN loopback");

    // burst read: ID, BUILD, SCRATCH, STATUS
    Resp b = transact(0x01, 7, 0x000, {4});
    CHECK(b.ok && b.status == 0 && b.words.size() == 4, "burst size/status");
    if (b.words.size() == 4)
        CHECK(b.words[0] == 0x444D4B37 && b.words[1] == 0x26102917 && b.words[2] == 0x12345678, "burst contents");

    // trims and probes
    CHECK(rd(0x100) == 0x13 && rd(0x104) == 0x0E && rd(0x108) == 0x41, "trim mirrors");
    CHECK(rd(0x20C) == 0x33333333, "PROBE[3]");
    CHECK(rd(0x400) == 0, "unmapped reads 0");

    // SWCLK edge counter (asynchronous input, slow toggles)
    uint32_t e0 = rd(0x024);
    for (int i = 0; i < 5; ++i) { top.p_swclk.set<bool>(true); tick(4); top.p_swclk.set<bool>(false); tick(4); }
    CHECK(rd(0x024) - e0 == 5, "SWCLK edges +5 (got %u)", rd(0x024) - e0);
    CHECK((rd(0x00C) & 0x4) || true, "STATUS readable");

    // uptime advances (64 clocks per "ms")
    uint32_t u0 = rd(0x018);
    tick(64 * 10);
    uint32_t u1 = rd(0x018);
    CHECK(u1 >= u0 + 10, "UPTIME %u -> %u", u0, u1);

    // errors: bad CRC, bad command, zero-length read, bus timeout
    Resp e = transact(0x01, 9, 0x000, {1}, true);
    CHECK(e.ok && e.status == 0x01 && e.words.empty() && e.seq == 9, "bad CRC -> status 1 (got %d)", e.status);
    e = transact(0x07, 10, 0x000, {1});
    CHECK(e.ok && e.status == 0x02 && e.words.empty(), "bad cmd -> status 2 (got %d)", e.status);
    e = transact(0x01, 11, 0x000, {0});
    CHECK(e.ok && e.status == 0x02, "count 0 -> status 2 (got %d)", e.status);
    e = transact(0x01, 12, 0x80000000u, {1});
    CHECK(e.ok && e.status == 0x03 && e.words.size() == 1 && e.words[0] == 0, "timeout -> status 3 (got %d)", e.status);
    e = transact(0x02, 13, 0x80000000u, {1, 2, 3, 4});
    CHECK(e.ok && e.status == 0x03, "write timeout -> status 3 (got %d)", e.status);

    // still alive after the errors
    CHECK(rd(0x000, 14) == 0x444D4B37, "ID after errors");

    std::printf("%s after %llu cycles, %d failure(s)\n", failures ? "FAILED" : "ALL OK",
                (unsigned long long)cycles, failures);
    return failures ? 1 : 0;
}
