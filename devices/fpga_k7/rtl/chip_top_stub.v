// chip_top_stub.v — the port list dm_platform_top expects from the chip RTL.
// Replace with (or adapt to) the real chip_top; this stub only lets the
// platform elaborate and returns fixed trim values.

`timescale 1ns / 1ps

module chip_top (
    input  wire         clk,        // DUT core clock (gated / divided by the platform)
    input  wire         por_n,      // power-on reset, active low (platform + DUT_RESET)
    input  wire         nrst,       // external reset pin from the 6571
    input  wire         swclk,
    input  wire         swdio_in,
    output wire         swdio_out,
    output wire         swdio_oe,
    input  wire [3:0]   pa_in,
    output wire [3:0]   pa_out,
    output wire [3:0]   pa_oe,
    input  wire         spi_sck,
    input  wire         spi_mosi,
    output wire         spi_miso,
    input  wire         spi_cs_n,
    input  wire [7:0]   mode,       // platform GPIO_OUT[7:0]: boot / test mode straps
    output wire [31:0]  status,     // -> platform GPIO_IN
    output wire [7:0]   trim_bg,    // trim register outputs, mirrored to the host
    output wire [7:0]   trim_ldo,
    output wire [7:0]   trim_osc,
    output wire [255:0] probes      // 8 x 32-bit internal probes
);
    assign swdio_out = 1'b0;
    assign swdio_oe  = 1'b0;
    assign pa_out    = 4'h0;
    assign pa_oe     = 4'h0;
    assign spi_miso  = 1'b0;
    assign status    = {24'h0, mode};
    assign trim_bg   = 8'h10;
    assign trim_ldo  = 8'h10;
    assign trim_osc  = 8'h40;
    assign probes    = 256'h0;
endmodule
