"""Simulated XC7K480T platform board.

* Byte-level model of rtl/dm_uart_bridge.v + dm_ctrl_regs.v (same framing, CRC,
  status codes, register map), used as the UART transport in simulation.
* Optional link to a virtual chip shared with the 6571 simulator (dm.shared):
  CTRL.DUT_RESET / DUT_CLK_EN stop the chip (SWD then fails), releasing the
  reset is a power-on reset, TRIM_* mirror the chip's trim registers and
  SWCLK_EDGES counts the 6571's SWCLK edges the chip saw.
* JTAG side: "programming" a bitstream reconfigures the board, XADC readings.
"""

import os
import random
import struct
import time

from uart_link import crc8

ID = 0x444D4B37
SYS_CLK = 200e6


class SimBoard:
    def __init__(self, options):
        self.rng = random.Random(options.get("seed"))
        self.shared_key = options.get("shared_dut")
        self.build_id = int(str(options.get("build_id", "0x26102917")), 0)
        self.configured = bool(options.get("configured", True))
        self.bitstream = options.get("bitstream", "sim_platform.bit") if self.configured else None
        self.temp0 = 38.0 + self.rng.random() * 4
        self._out = b""
        self._in = b""
        self._config()

    # ---- configuration -------------------------------------------------
    def _config(self):
        self.t0 = time.time()
        self.scratch = 0
        self.ctrl = 0x2
        self.clk_div = 3
        self.gpio_out = 0
        self.edges0 = self._chip_edges()
        self._apply_ctrl(power_on=True)

    def program(self, path):
        self.bitstream = os.path.basename(path)
        self.configured = True
        self._config()

    def xadc(self):
        load = 3.0 if self.dut_running() else 0.0
        return {"temperature": self.temp0 + load + self.rng.gauss(0, 0.15), "vccint": 1.0 + self.rng.gauss(0, 0.002),
                "vccaux": 1.8 + self.rng.gauss(0, 0.003), "vccbram": 1.0 + self.rng.gauss(0, 0.002)}

    # ---- link to the virtual chip --------------------------------------
    def chip(self):
        if not self.shared_key:
            return None
        import dm.shared
        return dm.shared.get(self.shared_key)

    def _chip_edges(self):
        c = self.chip()
        return getattr(c, "swclk_edges", 0) if c is not None else 0

    def dut_running(self):
        return bool(self.ctrl & 0x2) and not (self.ctrl & 0x1)

    def _apply_ctrl(self, power_on=False, was_running=True):
        c = self.chip()
        if c is None:
            return
        running = self.dut_running()
        if running and (power_on or not was_running) and hasattr(c, "power_on_reset"):
            c.power_on_reset()                     # releasing reset / starting the clock = POR
        c.enabled = running

    # ---- registers (dm_ctrl_regs) ----------------------------------------
    def reg_read(self, a):
        a &= 0xFFF
        c = self.chip()
        if a == 0x000:
            return ID
        if a == 0x004:
            return self.build_id
        if a == 0x008:
            return self.scratch
        if a == 0x00C:
            recent = 0
            if c is not None and getattr(c, "swclk_edges", 0) != getattr(self, "_last_edges", 0):
                recent = 1
                self._last_edges = c.swclk_edges
            return 1 | (int(self.dut_running()) << 1) | (recent << 2)
        if a == 0x010:
            return self.ctrl
        if a == 0x014:
            return self.clk_div
        if a == 0x018:
            return int((time.time() - self.t0) * 1000) & 0xFFFFFFFF
        if a == 0x01C:
            return self.gpio_out
        if a == 0x020:
            return 0x00010000 | (self.gpio_out & 0xFF) | (int(self.dut_running()) << 8)
        if a == 0x024:
            return (self._chip_edges() - self.edges0) & 0xFFFFFFFF
        if a in (0x100, 0x104, 0x108):
            if c is None:
                return {0x100: 0x10, 0x104: 0x10, 0x108: 0x40}[a]
            return c.trim[a - 0x100]                  # trim register outputs are always visible
        if 0x200 <= a < 0x220:
            if c is None:
                return 0
            probes = [c.regs[15] if hasattr(c, "regs") else 0, int(getattr(c, "halted", False)),
                      getattr(c, "swclk_edges", 0), 0, 0, 0, 0, 0]
            return probes[(a - 0x200) // 4] & 0xFFFFFFFF
        return 0

    def reg_write(self, a, v):
        a &= 0xFFF
        v &= 0xFFFFFFFF
        if a == 0x008:
            self.scratch = v
        elif a == 0x010:
            was = self.dut_running()
            self.ctrl = v & 0x3
            self._apply_ctrl(was_running=was)
        elif a == 0x014:
            self.clk_div = v & 0xFF
        elif a == 0x01C:
            self.gpio_out = v

    def dut_clock_hz(self):
        return SYS_CLK / (2 * (self.clk_div + 1))

    # ---- byte-level UART protocol (dm_uart_bridge) ---------------------
    def write(self, data):
        if not self.configured:
            return                                    # unconfigured FPGA: nobody listens
        self._in += bytes(data)
        while True:
            i = self._in.find(b"\xA5")
            if i < 0:
                self._in = b""
                return
            self._in = self._in[i:]
            if len(self._in) < 8:
                return
            cmd = self._in[1]
            n = 12 if cmd == 0x02 else 9               # A5 CMD SEQ ADDR[4] (DATA[4] | COUNT) CRC
            if len(self._in) < n:
                return
            frame, self._in = self._in[:n], self._in[n:]
            self._out += self._respond(frame)

    def _respond(self, f):
        cmd, seq = f[1], f[2]
        addr = struct.unpack("<I", f[3:7])[0]
        words, status = [], 0
        if crc8(f[1:-1]) != f[-1]:
            status = 1
        elif cmd == 0x01:
            count = f[7]
            if count == 0:
                status = 2
            else:
                words = [self.reg_read(addr + 4 * i) for i in range(count)]
        elif cmd == 0x02:
            self.reg_write(addr, struct.unpack("<I", f[7:11])[0])
        else:
            status = 2
        body = bytes([seq, len(words)]) + b"".join(struct.pack("<I", w) for w in words) + bytes([status])
        return b"\x5A" + body + bytes([crc8(body)])

    def read(self, n, timeout):
        if not self._out:
            time.sleep(min(timeout, 0.01))
        out, self._out = self._out[:n], self._out[n:]
        return out

    def flush_input(self):
        self._out = b""

    name = "simulated board"

    def close(self):
        pass
