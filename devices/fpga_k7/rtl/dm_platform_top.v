// dm_platform_top.v — XC7K480T "virtual chip" platform: wraps the chip RTL,
// exposes its SWD/GPIO/SPI pins to the PXIe-6571 (LVCMOS18) and adds the
// DeviceManager control plane (UART bridge + dm_ctrl_regs).
//
// Adapt the `chip_top` instance to your RTL's ports; chip_top_stub.v documents
// the expected interface and lets this file elaborate on its own.
// Pin locations: dm_platform_k7_1v8.xdc (fill in the TODOs for your board).

`timescale 1ns / 1ps

module dm_platform_top #(
    parameter [31:0]  BUILD_ID = 32'h0000_0000,
    parameter integer CLK_HZ   = 200_000_000,
    parameter integer BAUD     = 921_600
) (
    input  wire sys_clk,         // 200 MHz board oscillator
    input  wire sys_rst_n,       // board reset button, active low
    // host control plane (USB-UART)
    input  wire host_uart_rx,
    output wire host_uart_tx,
    // DUT pins towards the 6571 (1.8 V)
    input  wire swclk,
    inout  wire swdio,
    input  wire nrst,
    inout  wire [3:0] pa,
    input  wire spi_sck,
    input  wire spi_mosi,
    output wire spi_miso,
    input  wire spi_cs_n,
    // board LEDs (optional)
    output wire [3:0] led
);
    wire clk = sys_clk;
    // Clock-locked indication. With an MMCM, drive this from its LOCKED output.
    wire clk_locked = 1'b1;

    // power-on reset: hold for 16 cycles after configuration
    reg [15:0] por = 16'h0000;
    always @(posedge clk) por <= {por[14:0], sys_rst_n};
    wire rst = ~por[15];

    // ---------------------------------------------------------------- control plane
    wire [31:0] bus_addr, bus_wdata, bus_rdata;
    wire        bus_we, bus_re, bus_ack;
    wire        dut_reset, dut_clk_en;
    wire [7:0]  dut_clk_div;
    wire [31:0] gpio_out, gpio_in;
    wire [7:0]  trim_bg, trim_ldo, trim_osc;
    wire [255:0] probes;

    dm_uart_bridge #(.CLKS_PER_BIT(CLK_HZ / BAUD)) u_bridge (
        .clk(clk), .rst(rst), .uart_rx(host_uart_rx), .uart_tx(host_uart_tx),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_we(bus_we), .bus_re(bus_re),
        .bus_rdata(bus_rdata), .bus_ack(bus_ack));

    dm_ctrl_regs #(.BUILD_ID(BUILD_ID), .CLK_HZ(CLK_HZ)) u_regs (
        .clk(clk), .rst(rst),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_we(bus_we), .bus_re(bus_re),
        .bus_rdata(bus_rdata), .bus_ack(bus_ack),
        .clk_locked(clk_locked), .dut_reset(dut_reset), .dut_clk_en(dut_clk_en), .dut_clk_div(dut_clk_div),
        .gpio_out(gpio_out), .gpio_in(gpio_in), .swclk(swclk),
        .trim_bg(trim_bg), .trim_ldo(trim_ldo), .trim_osc(trim_osc), .probes(probes));

    // ---------------------------------------------------------------- DUT clock
    // sys_clk / (2 * (div + 1)), gated by DUT_CLK_EN.
    reg [7:0] div_cnt = 8'd0;
    reg       dut_clk_raw = 1'b0;
    always @(posedge clk) begin
        if (div_cnt >= dut_clk_div) begin
            div_cnt     <= 8'd0;
            dut_clk_raw <= ~dut_clk_raw;
        end else
            div_cnt <= div_cnt + 8'd1;
    end
    wire dut_clk;
`ifdef XILINX
    BUFGCE u_dut_clk (.I(dut_clk_raw), .CE(dut_clk_en), .O(dut_clk));
`else
    assign dut_clk = dut_clk_raw & dut_clk_en;
`endif

    // ---------------------------------------------------------------- the virtual chip
    wire swdio_out, swdio_oe;
    wire [3:0] pa_out, pa_oe;
    wire       por_n = ~rst & ~dut_reset;

    chip_top u_chip (
        .clk        (dut_clk),
        .por_n      (por_n),
        .nrst       (nrst),
        .swclk      (swclk),
        .swdio_in   (swdio),
        .swdio_out  (swdio_out),
        .swdio_oe   (swdio_oe),
        .pa_in      (pa),
        .pa_out     (pa_out),
        .pa_oe      (pa_oe),
        .spi_sck    (spi_sck),
        .spi_mosi   (spi_mosi),
        .spi_miso   (spi_miso),
        .spi_cs_n   (spi_cs_n),
        .mode       (gpio_out[7:0]),
        .status     (gpio_in),
        .trim_bg    (trim_bg),
        .trim_ldo   (trim_ldo),
        .trim_osc   (trim_osc),
        .probes     (probes)
    );

    // tri-state pads
    assign swdio = swdio_oe ? swdio_out : 1'bz;
    genvar g;
    generate
        for (g = 0; g < 4; g = g + 1) begin : pa_pads
            assign pa[g] = pa_oe[g] ? pa_out[g] : 1'bz;
        end
    endgenerate

    // LEDs: heartbeat, DUT running, SWCLK activity, reset
    reg [25:0] hb = 0;
    always @(posedge clk) hb <= hb + 26'd1;
    assign led = {dut_reset, swclk, dut_clk_en & ~dut_reset, hb[25]};
endmodule
