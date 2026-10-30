// dm_axil_adapter.v — AXI4-Lite slave to the dm_ctrl_regs bus, so the register block can
// also be reached by Vivado's JTAG-to-AXI Master (connect its M_AXI here). Writes and reads
// are handled one at a time; no bus_ack within 1023 clocks -> SLVERR.

`timescale 1ns / 1ps

module dm_axil_adapter (
    input  wire        clk,
    input  wire        rst,
    // AXI4-Lite slave
    input  wire [31:0] s_axi_awaddr,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,          // full-word writes only
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [31:0] s_axi_araddr,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,
    // simple bus master
    output reg  [31:0] bus_addr,
    output reg  [31:0] bus_wdata,
    output reg         bus_we,
    output reg         bus_re,
    input  wire [31:0] bus_rdata,
    input  wire        bus_ack
);
    localparam [2:0] IDLE = 3'd0, WWAIT = 3'd1, BRESP = 3'd2, RWAIT = 3'd3, RRESP = 3'd4;
    reg [2:0] state = IDLE;
    reg [9:0] timeout;

    always @(posedge clk) begin
        bus_we        <= 1'b0;
        bus_re        <= 1'b0;
        s_axi_awready <= 1'b0;
        s_axi_wready  <= 1'b0;
        s_axi_arready <= 1'b0;
        if (rst) begin
            state        <= IDLE;
            s_axi_bvalid <= 1'b0;
            s_axi_rvalid <= 1'b0;
        end else begin
            case (state)
            IDLE:
                if (s_axi_awvalid && s_axi_wvalid) begin
                    s_axi_awready <= 1'b1;
                    s_axi_wready  <= 1'b1;
                    bus_addr      <= s_axi_awaddr;
                    bus_wdata     <= s_axi_wdata;
                    bus_we        <= 1'b1;
                    timeout       <= 10'd0;
                    state         <= WWAIT;
                end else if (s_axi_arvalid) begin
                    s_axi_arready <= 1'b1;
                    bus_addr      <= s_axi_araddr;
                    bus_re        <= 1'b1;
                    timeout       <= 10'd0;
                    state         <= RWAIT;
                end
            WWAIT:
                if (bus_ack || timeout == 10'h3FF) begin
                    s_axi_bresp  <= bus_ack ? 2'b00 : 2'b10;
                    s_axi_bvalid <= 1'b1;
                    state        <= BRESP;
                end else
                    timeout <= timeout + 10'd1;
            BRESP:
                if (s_axi_bready) begin
                    s_axi_bvalid <= 1'b0;
                    state        <= IDLE;
                end
            RWAIT:
                if (bus_ack || timeout == 10'h3FF) begin
                    s_axi_rdata  <= bus_ack ? bus_rdata : 32'h0;
                    s_axi_rresp  <= bus_ack ? 2'b00 : 2'b10;
                    s_axi_rvalid <= 1'b1;
                    state        <= RRESP;
                end else
                    timeout <= timeout + 10'd1;
            RRESP:
                if (s_axi_rready) begin
                    s_axi_rvalid <= 1'b0;
                    state        <= IDLE;
                end
            default: state <= IDLE;
            endcase
        end
    end
endmodule
