// Simulation top: AXI4-Lite adapter + registers (bit 31 addresses never ack -> SLVERR).
`timescale 1ns / 1ps

module dm_axil_tb_top (
    input  wire        clk,
    input  wire        rst,
    input  wire [31:0] awaddr,  input wire awvalid, output wire awready,
    input  wire [31:0] wdata,   input wire wvalid,  output wire wready,
    output wire [1:0]  bresp,   output wire bvalid, input wire bready,
    input  wire [31:0] araddr,  input wire arvalid, output wire arready,
    output wire [31:0] rdata,   output wire [1:0] rresp, output wire rvalid, input wire rready,
    output wire        dut_reset
);
    wire [31:0] bus_addr, bus_wdata, regs_rdata;
    wire        bus_we, bus_re, regs_ack;

    dm_axil_adapter u_axi (
        .clk(clk), .rst(rst),
        .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_we(bus_we), .bus_re(bus_re),
        .bus_rdata(regs_rdata), .bus_ack(regs_ack & ~bus_addr[31]));

    dm_ctrl_regs #(.BUILD_ID(32'h2610_2917), .CLK_HZ(64_000)) u_regs (
        .clk(clk), .rst(rst),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_we(bus_we), .bus_re(bus_re),
        .bus_rdata(regs_rdata), .bus_ack(regs_ack),
        .clk_locked(1'b1), .dut_reset(dut_reset), .dut_clk_en(), .dut_clk_div(),
        .gpio_out(), .gpio_in(32'h0), .swclk(1'b0),
        .trim_bg(8'h13), .trim_ldo(8'h0E), .trim_osc(8'h41), .probes(256'h0));
endmodule
