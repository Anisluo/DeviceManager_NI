// Simulation top for the CXXRTL testbench: bridge + registers with short timings.
// Addresses with bit 31 set are never acknowledged (exercises the bus timeout).

`timescale 1ns / 1ps

module dm_bridge_tb_top (
    input  wire        clk,
    input  wire        rst,
    input  wire        uart_rx,
    output wire        uart_tx,
    input  wire        swclk,
    output wire        dut_reset,
    output wire        dut_clk_en,
    output wire [7:0]  dut_clk_div,
    output wire [31:0] gpio_out
);
    wire [31:0] bus_addr, bus_wdata, regs_rdata;
    wire        bus_we, bus_re, regs_ack;

    dm_uart_bridge #(.CLKS_PER_BIT(8)) u_bridge (
        .clk(clk), .rst(rst), .uart_rx(uart_rx), .uart_tx(uart_tx),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_we(bus_we), .bus_re(bus_re),
        .bus_rdata(regs_rdata), .bus_ack(regs_ack & ~bus_addr[31]));

    dm_ctrl_regs #(.BUILD_ID(32'h2610_2917), .CLK_HZ(64_000)) u_regs (      // 1 "ms" = 64 clocks
        .clk(clk), .rst(rst),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_we(bus_we), .bus_re(bus_re),
        .bus_rdata(regs_rdata), .bus_ack(regs_ack),
        .clk_locked(1'b1), .dut_reset(dut_reset), .dut_clk_en(dut_clk_en), .dut_clk_div(dut_clk_div),
        .gpio_out(gpio_out), .gpio_in(32'hCAFE_0000 | gpio_out[15:0]), .swclk(swclk),
        .trim_bg(8'h13), .trim_ldo(8'h0E), .trim_osc(8'h41),
        .probes({32'h7777_7777, 32'h6666_6666, 32'h5555_5555, 32'h4444_4444,
                 32'h3333_3333, 32'h2222_2222, 32'h1111_1111, 32'h0000_0000}));
endmodule
