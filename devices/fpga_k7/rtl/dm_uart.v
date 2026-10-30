// dm_uart.v — 8N1 UART receiver and transmitter.
// CLKS_PER_BIT = clk_hz / baud (e.g. 200 MHz / 921600 ≈ 217).

`timescale 1ns / 1ps

module dm_uart_rx #(
    parameter integer CLKS_PER_BIT = 217
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       rx,          // asynchronous line, idle high
    output reg  [7:0] data,
    output reg        valid        // one-cycle pulse per received byte
);
    // two-flop synchroniser
    reg rx_m = 1'b1, rx_s = 1'b1;
    always @(posedge clk) begin
        rx_m <= rx;
        rx_s <= rx_m;
    end

    localparam [1:0] IDLE = 2'd0, START = 2'd1, BITS = 2'd2, STOP = 2'd3;
    reg [1:0]  state = IDLE;
    reg [15:0] cnt   = 16'd0;
    reg [2:0]  idx   = 3'd0;
    reg [7:0]  shift = 8'd0;

    always @(posedge clk) begin
        valid <= 1'b0;
        if (rst) begin
            state <= IDLE;
            cnt   <= 16'd0;
            idx   <= 3'd0;
        end else begin
            case (state)
            IDLE:
                if (!rx_s) begin                      // falling edge: start bit
                    state <= START;
                    cnt   <= 16'd0;
                end
            START:                                    // re-check in the middle of the start bit
                if (cnt == CLKS_PER_BIT / 2 - 1) begin
                    cnt   <= 16'd0;
                    state <= rx_s ? IDLE : BITS;      // glitch -> back to idle
                    idx   <= 3'd0;
                end else
                    cnt <= cnt + 16'd1;
            BITS:
                if (cnt == CLKS_PER_BIT - 1) begin
                    cnt        <= 16'd0;
                    shift[idx] <= rx_s;               // LSB first
                    if (idx == 3'd7)
                        state <= STOP;
                    idx <= idx + 3'd1;
                end else
                    cnt <= cnt + 16'd1;
            STOP:
                if (cnt == CLKS_PER_BIT - 1) begin
                    cnt   <= 16'd0;
                    state <= IDLE;
                    if (rx_s) begin                   // valid stop bit
                        data  <= shift;
                        valid <= 1'b1;
                    end
                end else
                    cnt <= cnt + 16'd1;
            endcase
        end
    end
endmodule


module dm_uart_tx #(
    parameter integer CLKS_PER_BIT = 217
) (
    input  wire       clk,
    input  wire       rst,
    input  wire [7:0] data,
    input  wire       valid,       // accepted when ready
    output wire       ready,
    output reg        tx = 1'b1
);
    reg [9:0]  frame = 10'h3FF;    // {stop, data[7:0], start}, shifted out LSB first
    reg [3:0]  left  = 4'd0;       // bits still to send
    reg [15:0] cnt   = 16'd0;

    assign ready = (left == 4'd0);

    always @(posedge clk) begin
        if (rst) begin
            tx   <= 1'b1;
            left <= 4'd0;
            cnt  <= 16'd0;
        end else if (left == 4'd0) begin
            if (valid) begin
                frame <= {1'b1, data, 1'b0};
                left  <= 4'd10;
                cnt   <= 16'd0;
                tx    <= 1'b0;                      // start bit goes out immediately
            end
        end else if (cnt == CLKS_PER_BIT - 1) begin
            cnt   <= 16'd0;
            frame <= {1'b1, frame[9:1]};
            left  <= left - 4'd1;
            tx    <= (left == 4'd1) ? 1'b1 : frame[1];
        end else
            cnt <= cnt + 16'd1;
    end
endmodule
