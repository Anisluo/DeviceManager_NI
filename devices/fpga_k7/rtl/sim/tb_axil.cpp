// CXXRTL testbench for dm_axil_adapter (AXI4-Lite handshakes, SLVERR on timeout).
#include "axil_top.cc"

#include <cstdint>
#include <cstdio>

static cxxrtl_design::p_dm__axil__tb__top top;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

static void tick()
{
    top.p_clk.set<bool>(false); top.step();
    top.p_clk.set<bool>(true);  top.step();
}

static uint32_t axi_read(uint32_t a, unsigned* resp)
{
    top.p_araddr.set<uint32_t>(a);
    top.p_arvalid.set<bool>(true);
    for (int i = 0; i < 50 && !top.p_arready.get<bool>(); ++i) tick();
    tick();
    top.p_arvalid.set<bool>(false);
    top.p_rready.set<bool>(true);
    int n = 0;
    while (!top.p_rvalid.get<bool>() && n++ < 2000) tick();
    uint32_t d = top.p_rdata.get<uint32_t>();
    *resp = top.p_rresp.get<unsigned>();
    tick();
    top.p_rready.set<bool>(false);
    CHECK(!top.p_rvalid.get<bool>(), "rvalid dropped after handshake");
    return d;
}

static unsigned axi_write(uint32_t a, uint32_t v)
{
    top.p_awaddr.set<uint32_t>(a);
    top.p_wdata.set<uint32_t>(v);
    top.p_awvalid.set<bool>(true);
    top.p_wvalid.set<bool>(true);
    for (int i = 0; i < 50 && !top.p_awready.get<bool>(); ++i) tick();
    tick();
    top.p_awvalid.set<bool>(false);
    top.p_wvalid.set<bool>(false);
    top.p_bready.set<bool>(true);
    int n = 0;
    while (!top.p_bvalid.get<bool>() && n++ < 2000) tick();
    unsigned r = top.p_bresp.get<unsigned>();
    tick();
    top.p_bready.set<bool>(false);
    return r;
}

int main()
{
    top.p_rst.set<bool>(true); tick(); tick();
    top.p_rst.set<bool>(false); tick();
    unsigned resp;
    CHECK(axi_read(0x000, &resp) == 0x444D4B37 && resp == 0, "ID over AXI");
    CHECK(axi_write(0x008, 0xA1B2C3D4) == 0, "write SCRATCH");
    CHECK(axi_read(0x008, &resp) == 0xA1B2C3D4 && resp == 0, "read SCRATCH");
    CHECK(axi_write(0x010, 0x1) == 0, "write CTRL");
    tick();
    CHECK(top.p_dut__reset.get<bool>(), "dut_reset follows CTRL");
    CHECK(axi_read(0x100, &resp) == 0x13, "TRIM_BG");
    axi_read(0x80000000u, &resp);
    CHECK(resp == 2, "read timeout -> SLVERR (got %u)", resp);
    CHECK(axi_write(0x80000000u, 1) == 2, "write timeout -> SLVERR");
    CHECK(axi_read(0x000, &resp) == 0x444D4B37 && resp == 0, "alive after SLVERR");
    std::printf("%s, %d failure(s)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures ? 1 : 0;
}
