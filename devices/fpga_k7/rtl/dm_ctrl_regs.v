// dm_ctrl_regs.v — platform control / status registers behind dm_uart_bridge
// (or a JTAG-to-AXI adapter). Every access is acknowledged one cycle later;
// unmapped addresses read 0 and ignore writes. Keep this map in sync with
// devices/fpga_k7/regmap.json.
//
//  0x000 ID          RO  0x444D4B37 ("DMK7")
//  0x004 BUILD       RO  BUILD_ID parameter (e.g. 0xYYMMDDHH)
//  0x008 SCRATCH     RW
//  0x00C STATUS      RO  [0] clk_locked  [1] dut_running  [2] swclk seen in the last ~1 ms
//  0x010 CTRL        RW  [0] DUT_RESET (1 = hold the virtual chip in reset)  [1] DUT_CLK_EN   reset: 0x2
//  0x014 CLK_DIV     RW  [7:0] DUT clock = sys_clk / (2 * (CLK_DIV + 1))
//  0x018 UPTIME_MS   RO  milliseconds since configuration
//  0x01C GPIO_OUT    RW  to the virtual chip (boot mode, test mode, ...)
//  0x020 GPIO_IN     RO  from the virtual chip
//  0x024 SWCLK_EDGES RO  SWCLK rising edges seen from the 6571 (wiring / level check)
//  0x100 TRIM_BG     RO  mirrors of the chip's trim registers (analog stubs on the host)
//  0x104 TRIM_LDO    RO
//  0x108 TRIM_OSC    RO
//  0x200..0x21C PROBE[0..7] RO  free 32-bit probes into the chip

`timescale 1ns / 1ps

module dm_ctrl_regs #(
    parameter [31:0]  BUILD_ID = 32'h0000_0000,
    parameter integer CLK_HZ   = 200_000_000
) (
    input  wire         clk,
    input  wire         rst,
    // bus slave (from dm_uart_bridge)
    input  wire [31:0]  bus_addr,
    input  wire [31:0]  bus_wdata,
    input  wire         bus_we,
    input  wire         bus_re,
    output reg  [31:0]  bus_rdata,
    output reg          bus_ack,
    // platform
    input  wire         clk_locked,
    output wire         dut_reset,
    output wire         dut_clk_en,
    output wire [7:0]   dut_clk_div,
    output reg  [31:0]  gpio_out,
    input  wire [31:0]  gpio_in,
    input  wire         swclk,          // asynchronous, from the 6571 pin
    input  wire [7:0]   trim_bg,
    input  wire [7:0]   trim_ldo,
    input  wire [7:0]   trim_osc,
    input  wire [255:0] probes          // 8 x 32 bit
);
    reg [31:0] scratch;
    reg [1:0]  ctrl;
    reg [7:0]  clk_div;
    assign dut_reset   = ctrl[0];
    assign dut_clk_en  = ctrl[1];
    assign dut_clk_div = clk_div;

    // millisecond tick / uptime
    localparam integer MS = CLK_HZ / 1000;
    reg [31:0] ms_cnt = 0;
    reg [31:0] uptime = 0;
    wire       ms_tick = (ms_cnt == MS - 1);
    always @(posedge clk) begin
        ms_cnt <= ms_tick ? 32'd0 : ms_cnt + 32'd1;
        if (ms_tick) uptime <= uptime + 32'd1;
    end

    // SWCLK activity (synchronise, count rising edges, "seen recently" flag)
    reg [2:0]  sw_sync = 3'b000;
    reg [31:0] sw_edges = 0;
    reg        sw_recent = 1'b0, sw_this_ms = 1'b0;
    always @(posedge clk) begin
        sw_sync <= {sw_sync[1:0], swclk};
        if (sw_sync[2:1] == 2'b01) begin
            sw_edges   <= sw_edges + 32'd1;
            sw_this_ms <= 1'b1;
        end
        if (ms_tick) begin
            sw_recent  <= sw_this_ms;
            sw_this_ms <= 1'b0;
        end
    end

    always @(posedge clk) begin
        bus_ack <= bus_we | bus_re;
        if (rst) begin
            scratch  <= 32'h0;
            ctrl     <= 2'b10;          // clock running, chip out of reset
            clk_div  <= 8'd3;           // 200 MHz / 8 = 25 MHz
            gpio_out <= 32'h0;
        end else if (bus_we) begin
            case (bus_addr[11:0])
            12'h008: scratch  <= bus_wdata;
            12'h010: ctrl     <= bus_wdata[1:0];
            12'h014: clk_div  <= bus_wdata[7:0];
            12'h01C: gpio_out <= bus_wdata;
            default: ;
            endcase
        end
        if (bus_re) begin
            casez (bus_addr[11:0])
            12'h000: bus_rdata <= 32'h444D_4B37;
            12'h004: bus_rdata <= BUILD_ID;
            12'h008: bus_rdata <= scratch;
            12'h00C: bus_rdata <= {29'd0, sw_recent, dut_clk_en & ~dut_reset & clk_locked, clk_locked};
            12'h010: bus_rdata <= {30'd0, ctrl};
            12'h014: bus_rdata <= {24'd0, clk_div};
            12'h018: bus_rdata <= uptime;
            12'h01C: bus_rdata <= gpio_out;
            12'h020: bus_rdata <= gpio_in;
            12'h024: bus_rdata <= sw_edges;
            12'h100: bus_rdata <= {24'd0, trim_bg};
            12'h104: bus_rdata <= {24'd0, trim_ldo};
            12'h108: bus_rdata <= {24'd0, trim_osc};
            12'h2??: bus_rdata <= (bus_addr[7:0] < 8'h20) ? probes[bus_addr[4:2] * 32 +: 32] : 32'h0;
            default: bus_rdata <= 32'h0;
            endcase
        end
    end
endmodule
