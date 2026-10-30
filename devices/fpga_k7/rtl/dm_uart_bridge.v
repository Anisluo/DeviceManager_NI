// dm_uart_bridge.v — host register access over UART (DeviceManager fpga_k7 plugin).
//
// Request  : A5 | CMD | SEQ | ADDR[4] (LE) | ARG | CRC8
//            CMD 01 READ  : ARG = COUNT (1 byte, 1..255 words, address +4 per word)
//            CMD 02 WRITE : ARG = DATA[4] (LE)
// Response : 5A | SEQ | N | DATA[4*N] (LE) | STATUS | CRC8
//            N = words returned (COUNT for a good READ, else 0)
//            STATUS 00 OK, 01 CRC error, 02 bad command, 03 bus timeout
// CRC8     : polynomial 0x07, init 0x00, MSB first (CRC-8/SMBUS), over every byte
//            after the sync byte (request: CMD..ARG, response: SEQ..STATUS).
//
// Bus      : single-cycle-strobe master. Assert bus_re / bus_we for one clock with
//            bus_addr (and bus_wdata); the slave answers with a one-cycle bus_ack
//            (bus_rdata valid in that cycle). No ack within 1023 clocks -> timeout.

`timescale 1ns / 1ps

module dm_uart_bridge #(
    parameter integer CLKS_PER_BIT = 217
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        uart_rx,
    output wire        uart_tx,
    output reg  [31:0] bus_addr,
    output reg  [31:0] bus_wdata,
    output reg         bus_we,
    output reg         bus_re,
    input  wire [31:0] bus_rdata,
    input  wire        bus_ack
);
    // ---------------------------------------------------------------- UART
    wire [7:0] rx_data;
    wire       rx_valid;
    reg  [7:0] tx_data;
    reg        tx_valid;
    wire       tx_ready;

    dm_uart_rx #(.CLKS_PER_BIT(CLKS_PER_BIT)) u_rx (
        .clk(clk), .rst(rst), .rx(uart_rx), .data(rx_data), .valid(rx_valid));
    dm_uart_tx #(.CLKS_PER_BIT(CLKS_PER_BIT)) u_tx (
        .clk(clk), .rst(rst), .data(tx_data), .valid(tx_valid), .ready(tx_ready), .tx(uart_tx));

    function [7:0] crc8;                              // one byte into CRC-8 (poly 0x07)
        input [7:0] crc_in;
        input [7:0] d;
        integer i;
        reg [7:0] c;
        begin
            c = crc_in ^ d;
            for (i = 0; i < 8; i = i + 1)
                c = c[7] ? ((c << 1) ^ 8'h07) : (c << 1);
            crc8 = c;
        end
    endfunction

    // ---------------------------------------------------------------- state
    localparam [4:0]
        R_SYNC = 5'd0, R_CMD = 5'd1, R_SEQ = 5'd2, R_ADDR = 5'd3, R_ARG = 5'd4, R_CRC = 5'd5,
        X_WRITE = 5'd6, X_WWAIT = 5'd7,
        T_SYNC = 5'd8, T_SEQ = 5'd9, T_N = 5'd10,
        X_READ = 5'd11, X_RWAIT = 5'd12, T_DATA = 5'd13,
        T_STATUS = 5'd14, T_CRC = 5'd15, T_WAIT = 5'd16;

    reg [4:0]  state = R_SYNC;
    reg [4:0]  after_tx;          // state to enter once the queued byte has gone out
    reg [7:0]  cmd, seq, count, left, status, crc;
    reg [31:0] addr, wdata, rword;
    reg [2:0]  nbyte;             // byte index within a multi-byte field
    reg [9:0]  timeout;

    // queue one response byte, update the CRC, continue in state `next`
    task send;
        input [7:0] b;
        input       with_crc;
        input [4:0] next;
        begin
            tx_data  <= b;
            tx_valid <= 1'b1;
            if (with_crc) crc <= crc8(crc, b);
            after_tx <= next;
            state    <= T_WAIT;
        end
    endtask

    always @(posedge clk) begin
        bus_we   <= 1'b0;
        bus_re   <= 1'b0;
        tx_valid <= 1'b0;
        if (rst) begin
            state <= R_SYNC;
        end else begin
            case (state)
            // ---------------- receive a request
            R_SYNC: if (rx_valid && rx_data == 8'hA5) begin
                        crc   <= 8'h00;
                        state <= R_CMD;
                    end
            R_CMD:  if (rx_valid) begin
                        cmd   <= rx_data;
                        crc   <= crc8(crc, rx_data);
                        state <= R_SEQ;
                    end
            R_SEQ:  if (rx_valid) begin
                        seq   <= rx_data;
                        crc   <= crc8(crc, rx_data);
                        nbyte <= 3'd0;
                        state <= R_ADDR;
                    end
            R_ADDR: if (rx_valid) begin
                        addr  <= {rx_data, addr[31:8]};    // little endian
                        crc   <= crc8(crc, rx_data);
                        nbyte <= nbyte + 3'd1;
                        if (nbyte == 3'd3) begin
                            nbyte <= 3'd0;
                            state <= R_ARG;
                        end
                    end
            R_ARG:  if (rx_valid) begin
                        crc <= crc8(crc, rx_data);
                        if (cmd == 8'h02) begin
                            wdata <= {rx_data, wdata[31:8]};
                            nbyte <= nbyte + 3'd1;
                            if (nbyte == 3'd3) state <= R_CRC;
                        end else begin
                            count <= rx_data;
                            state <= R_CRC;
                        end
                    end
            R_CRC:  if (rx_valid) begin
                        // decide; the response CRC restarts at 0
                        if (rx_data != crc)                          status <= 8'h01;
                        else if (cmd != 8'h01 && cmd != 8'h02)       status <= 8'h02;
                        else if (cmd == 8'h01 && count == 8'h00)     status <= 8'h02;
                        else                                         status <= 8'h00;
                        crc <= 8'h00;
                        if (rx_data == crc && cmd == 8'h02)
                            state <= X_WRITE;
                        else
                            state <= T_SYNC;
                    end
            // ---------------- execute a write before answering
            X_WRITE: begin
                        bus_addr  <= addr;
                        bus_wdata <= wdata;
                        bus_we    <= 1'b1;
                        timeout   <= 10'd0;
                        state     <= X_WWAIT;
                     end
            X_WWAIT: if (bus_ack)
                        state <= T_SYNC;
                     else if (timeout == 10'h3FF) begin
                        status <= 8'h03;
                        state  <= T_SYNC;
                     end else
                        timeout <= timeout + 10'd1;
            // ---------------- response header
            T_SYNC:  send(8'h5A, 1'b0, T_SEQ);
            T_SEQ:   send(seq, 1'b1, T_N);
            T_N:     begin
                        left <= (status == 8'h00 && cmd == 8'h01) ? count : 8'h00;
                        send((status == 8'h00 && cmd == 8'h01) ? count : 8'h00, 1'b1,
                             (status == 8'h00 && cmd == 8'h01) ? X_READ : T_STATUS);
                     end
            // ---------------- read words one by one, streaming them out
            X_READ:  begin
                        bus_addr <= addr;
                        bus_re   <= 1'b1;
                        timeout  <= 10'd0;
                        state    <= X_RWAIT;
                     end
            X_RWAIT: if (bus_ack) begin
                        rword <= bus_rdata;
                        nbyte <= 3'd0;
                        state <= T_DATA;
                     end else if (timeout == 10'h3FF) begin
                        rword  <= 32'h0;
                        status <= 8'h03;
                        nbyte  <= 3'd0;
                        state  <= T_DATA;
                     end else
                        timeout <= timeout + 10'd1;
            T_DATA:  begin
                        nbyte <= nbyte + 3'd1;
                        rword <= {8'h00, rword[31:8]};
                        if (nbyte == 3'd3) begin
                            left <= left - 8'd1;
                            addr <= addr + 32'd4;
                            send(rword[7:0], 1'b1, (left == 8'd1) ? T_STATUS : X_READ);
                        end else
                            send(rword[7:0], 1'b1, T_DATA);
                     end
            T_STATUS: send(status, 1'b1, T_CRC);
            T_CRC:    send(crc, 1'b0, R_SYNC);
            // wait until the transmitter has taken the byte and is free again
            T_WAIT:   if (tx_ready && !tx_valid)
                        state <= after_tx;
            default:  state <= R_SYNC;
            endcase
        end
    end
endmodule
