"""Virtual Cortex-M0 test chip for the 6571 simulator.

* Electrical model of every signal pin (ESD diodes, pull-up/down, leakage,
  injectable opens / shorts) so PPMU, continuity and leakage tests behave
  like silicon.
* Bit-level SWD target: request parsing, ACK, turnaround, parity, line reset,
  DP (IDCODE / CTRL/STAT / SELECT / RDBUFF / ABORT), MEM-AP (CSW / TAR / DRW,
  posted reads, auto-increment), Cortex-M0 debug (DHCSR / DCRSR / DCRDR /
  DEMCR / AIRCR / CPUID), flash, SRAM, a chip-ID block and trim registers.
* Functional window: input thresholds 0.3/0.7·VDD, output high = VDD, and a
  maximum SWCLK frequency that falls with VDD and with marginal drive levels,
  so level × timing shmoos have a realistic shape.
"""

import math
import random

VT = 0.025852

# Chip specifics (the real values come from the design database)
IDCODE = 0x0BB11477             # Cortex-M0 SW-DP
AP_IDR = 0x04770021             # AHB-AP
CPUID_VAL = 0x410CC200          # Cortex-M0 r0p0
CHIP_ID = 0x6A5C0001
FLASH_BASE, FLASH_SIZE = 0x00000000, 64 * 1024
SRAM_BASE, SRAM_SIZE = 0x20000000, 8 * 1024
SYSCFG = 0x40000000             # +0 CHIPID, +4 REVISION
TRIM = 0x40001000               # +0 BG[4:0], +4 LDO[4:0], +8 OSC[6:0], +C LOCK (write 0xA5)
TRIM_DEFAULTS = {0x0: 0x10, 0x4: 0x10, 0x8: 0x40}
TRIM_MASKS = {0x0: 0x1F, 0x4: 0x1F, 0x8: 0x7F}

# Default pin set: name -> (pull, resistance)
DEFAULT_PINS = {
    "SWCLK": ("down", 100e3), "SWDIO": ("up", 40e3), "nRST": ("up", 40e3),
    "PA0": (None, 0), "PA1": (None, 0), "PA2": (None, 0), "PA3": (None, 0),
    "SPI_SCK": (None, 0), "SPI_MOSI": (None, 0), "SPI_MISO": (None, 0), "SPI_CS": ("up", 40e3),
}


class BusError(Exception):
    pass


class Pin:
    def __init__(self, name, pull=None, r_pull=0.0, rng=None):
        rng = rng or random
        self.name = name
        self.pull = pull
        self.r_pull = r_pull
        self.is_esd = 1e-14 * (1 + 0.2 * rng.random())       # diode saturation current
        self.n_esd = 1.05
        self.i_leak = (1 + 3 * rng.random()) * 1e-9           # junction leakage at 1 V
        self.fault = None                                     # None | open | short_gnd | short_vdd | short:<pin> | leaky

    def current(self, v, vdd, other_v=None):
        """Current flowing from the tester INTO the pin at voltage v."""
        if self.fault == "open":
            return 0.2e-9 * v                                 # only the tester's own leakage
        i = self.i_leak * v
        nvt = self.n_esd * VT
        i -= self.is_esd * (math.exp(min(-v / nvt, 80.0)) - 1)        # diode to GND (conducts for v < 0)
        i += self.is_esd * (math.exp(min((v - vdd) / nvt, 80.0)) - 1)  # diode to VDD (conducts for v > VDD)
        if self.pull == "up" and vdd > 0.5:
            i += (v - vdd) / self.r_pull
        elif self.pull == "down":
            i += v / self.r_pull
        if self.fault == "short_gnd":
            i += v / 5.0
        elif self.fault == "short_vdd":
            i += (v - vdd) / 5.0
        elif self.fault and self.fault.startswith("short:") and other_v is not None:
            i += (v - other_v) / 5.0
        elif self.fault == "leaky":
            i += 2e-6 * v
        return i


class SwdTarget:
    """SW-DP + AHB-AP + Cortex-M0 debug, clocked one SWCLK cycle at a time."""

    def __init__(self, chip):
        self.chip = chip
        self.ones = 0
        self.state = "reset"           # reset | idle | req | trn1 | ack | rdata | trn2 | wdata
        self.bits = []
        self.req = None
        self.ack = 0
        self.out = []                  # bits still to drive
        self.idcode_read = False
        self.ctrl = 0
        self.select = 0
        self.rdbuff = 0
        self.sticky = 0
        # MEM-AP
        self.csw = 0x03000040
        self.tar = 0

    def line_reset(self):
        self.state = "reset"
        self.idcode_read = False

    def cycle(self, line):
        """One SWCLK cycle. `line` is the SWDIO level seen at the rising edge (None = floating).
        Returns the bit this target drives during the cycle, or None."""
        self.chip.swclk_edges += 1                            # seen by the FPGA platform's edge counter
        bit = 1 if line is None else line
        self.ones = self.ones + 1 if bit == 1 else 0
        if self.ones >= 50:
            self.line_reset()
            return None

        st = self.state
        if st == "reset":
            if bit == 0:
                self.state = "idle"
            return None
        if st == "idle":
            if bit == 1:
                self.state, self.bits = "req", [1]
            return None
        if st == "req":
            self.bits.append(bit)
            if len(self.bits) == 8:
                self._decode()
            return None
        if st == "trn1":
            self.state = "ack"
            self.out = [(self.ack >> i) & 1 for i in range(3)]
            return None
        if st == "ack":
            b = self.out.pop(0)
            if not self.out:
                apndp, rnw, addr = self.req
                if self.ack == 0b001 and rnw:
                    value = self._read(apndp, addr)
                    self.out = [(value >> i) & 1 for i in range(32)] + [bin(value).count("1") & 1]
                    self.state = "rdata"
                else:
                    self.state = "trn2"
                    self.next_after_trn = "wdata" if (self.ack == 0b001 and not rnw) else "idle"
                    self.bits = []
            return b
        if st == "rdata":
            b = self.out.pop(0)
            if not self.out:
                self.state, self.next_after_trn = "trn2", "idle"
            return b
        if st == "trn2":
            self.state = self.next_after_trn
            return None
        if st == "wdata":
            self.bits.append(bit)
            if len(self.bits) == 33:
                value = sum(b << i for i, b in enumerate(self.bits[:32]))
                if self.bits[32] == (bin(value).count("1") & 1):
                    apndp, _, addr = self.req
                    self._write(apndp, addr, value)
                else:
                    self.sticky |= 0x80                       # WDATAERR
                self.state = "idle"
            return None
        return None

    def _decode(self):
        b = self.bits
        start, apndp, rnw, a2, a3, par, stop, park = b
        if start != 1 or stop != 0 or park != 1 or par != ((apndp + rnw + a2 + a3) & 1):
            self.state = "idle"                               # protocol error: no response
            return
        addr = (a3 << 3) | (a2 << 2)
        self.req = (apndp, rnw, addr)
        if not self.idcode_read and not (apndp == 0 and rnw == 1 and addr == 0):
            self.state = "idle"                               # must read IDCODE after a line reset
            return
        if apndp and not (self.ctrl & (1 << 28)):
            self.ack = 0b100                                  # AP access without debug power: FAULT
        elif apndp and self.sticky & 0x22:
            self.ack = 0b100                                  # sticky error set
        else:
            self.ack = 0b001
        self.state = "trn1"

    # ---- DP / AP registers ----
    def _read(self, apndp, addr):
        if not apndp:
            if addr == 0x0:
                self.idcode_read = True
                return IDCODE
            if addr == 0x4:
                acks = (self.ctrl << 1) & 0xA0000000          # power-up REQ bits 30/28 -> ACK bits 31/29
                return (self.ctrl & 0x50000F00) | acks | self.sticky
            if addr == 0x8:
                return self.rdbuff
            return self.rdbuff                                # RDBUFF
        value = self._ap_read(((self.select & 0xF0) | addr))
        posted, self.rdbuff = self.rdbuff, value
        return posted

    def _write(self, apndp, addr, value):
        if not apndp:
            if addr == 0x0:
                if value & 0x1E:
                    self.sticky = 0
            elif addr == 0x4:
                self.ctrl = value & 0x50000F00
            elif addr == 0x8:
                self.select = value
            return
        self._ap_write((self.select & 0xF0) | addr, value)

    def _ap_read(self, reg):
        if (self.select >> 24) != 0:
            return 0                                          # only AP #0 exists
        if reg == 0x00:
            return self.csw
        if reg == 0x04:
            return self.tar
        if reg == 0x0C:
            try:
                v = self.chip.bus_read(self.tar)
            except BusError:
                self.sticky |= 0x20                           # STICKYERR
                v = 0
            self._inc()
            return v
        if reg == 0xF8:
            return 0xE00FF003
        if reg == 0xFC:
            return AP_IDR
        return 0

    def _ap_write(self, reg, value):
        if (self.select >> 24) != 0:
            return
        if reg == 0x00:
            self.csw = (self.csw & ~0x37) | (value & 0x37) | (value & 0xFF000000)
        elif reg == 0x04:
            self.tar = value
        elif reg == 0x0C:
            try:
                self.chip.bus_write(self.tar, value)
            except BusError:
                self.sticky |= 0x20
            self._inc()

    def _inc(self):
        if (self.csw >> 4) & 0x3 == 1:                        # AddrInc single
            self.tar = (self.tar & ~0x3FF) | ((self.tar + 4) & 0x3FF)


class SimChip:
    """Memory map + Cortex-M0 core state + pins."""

    def __init__(self, seed=None, pins=None):
        self.rng = random.Random(seed)
        self.vdd = 3.3
        self.pins = {name: Pin(name, *(pins or DEFAULT_PINS)[name], rng=self.rng) for name in (pins or DEFAULT_PINS)}
        self.flash = bytearray(b"\xff" * FLASH_SIZE)
        self.sram = bytearray(self.rng.getrandbits(8) for _ in range(SRAM_SIZE))
        # vector table: initial SP, reset handler (thumb), NMI, HardFault
        for i, v in enumerate([SRAM_BASE + SRAM_SIZE, 0x000000C1, 0x000000D1, 0x000000D3]):
            self.flash[4 * i:4 * i + 4] = v.to_bytes(4, "little")
        self.trim = dict(TRIM_DEFAULTS)
        self.trim_locked = False
        self.swd = SwdTarget(self)
        self.enabled = True          # False while an emulation platform holds the chip in reset / stops its clock
        self.swclk_edges = 0
        self._reset_core()
        self.c_debugen = False

    # ---- core ----
    def _reset_core(self):
        self.regs = [0] * 19
        self.regs[13] = int.from_bytes(self.flash[0:4], "little")
        self.regs[15] = int.from_bytes(self.flash[4:8], "little") & ~1
        self.regs[16] = 0x01000000
        self.halted = getattr(self, "vc_corereset", False) and getattr(self, "c_debugen", False)
        self.reset_seen = True
        self.regrdy = True
        if not self.trim_locked:
            self.trim = dict(TRIM_DEFAULTS)

    def tick(self):
        if not self.halted:                                   # the core "runs": PC moves
            self.regs[15] = 0xC0 + (self.rng.randrange(0, 64) & ~1)

    # ---- bus ----
    def bus_read(self, a):
        a &= 0xFFFFFFFC
        self.tick()
        if FLASH_BASE <= a < FLASH_BASE + FLASH_SIZE:
            return int.from_bytes(self.flash[a:a + 4], "little")
        if SRAM_BASE <= a < SRAM_BASE + SRAM_SIZE:
            o = a - SRAM_BASE
            return int.from_bytes(self.sram[o:o + 4], "little")
        if a == SYSCFG:
            return CHIP_ID
        if a == SYSCFG + 4:
            return 0x00000002                                 # revision B
        if TRIM <= a < TRIM + 0x10:
            o = a - TRIM
            return 1 if (o == 0xC and self.trim_locked) else self.trim.get(o, 0)
        if a == 0xE000ED00:
            return CPUID_VAL
        if a == 0xE000EDF0:                                   # DHCSR
            v = (int(self.c_debugen) | (int(self.halted) << 1) | (int(self.regrdy) << 16)
                 | (int(self.halted) << 17) | (int(self.reset_seen) << 25))
            self.reset_seen = False
            return v
        if a == 0xE000EDF8:
            return self.dcrdr
        if a == 0xE000EDFC:
            return int(getattr(self, "vc_corereset", False))
        if a == 0xE000ED0C:
            return 0xFA050000
        if 0xE0000000 <= a < 0xE0100000:
            return 0
        raise BusError(hex(a))

    def bus_write(self, a, v):
        a &= 0xFFFFFFFC
        v &= 0xFFFFFFFF
        if SRAM_BASE <= a < SRAM_BASE + SRAM_SIZE:
            o = a - SRAM_BASE
            self.sram[o:o + 4] = v.to_bytes(4, "little")
            return
        if FLASH_BASE <= a < FLASH_BASE + FLASH_SIZE:
            raise BusError("flash is not writable through the bus")
        if TRIM <= a < TRIM + 0x10:
            o = a - TRIM
            if o == 0xC:
                self.trim_locked = self.trim_locked or (v == 0xA5)
            elif not self.trim_locked:
                self.trim[o] = v & TRIM_MASKS[o]
            return
        if a == 0xE000EDF0:
            if (v >> 16) == 0xA05F:
                self.c_debugen = bool(v & 1)
                if self.c_debugen:
                    self.halted = bool(v & 2)
            return
        if a == 0xE000EDF4:                                   # DCRSR
            sel = v & 0x1F
            if sel < len(self.regs):
                if v & (1 << 16):
                    self.regs[sel] = self.dcrdr
                else:
                    self.dcrdr = self.regs[sel] if self.halted else 0
            self.regrdy = True
            return
        if a == 0xE000EDF8:
            self.dcrdr = v
            return
        if a == 0xE000EDFC:
            self.vc_corereset = bool(v & 1)
            return
        if a == 0xE000ED0C:
            if (v >> 16) == 0x05FA and v & 0x4:
                self._reset_core()
            return
        if 0xE0000000 <= a < 0xE0100000:
            return
        raise BusError(hex(a))

    dcrdr = 0

    # ---- functional window ----
    def power_on_reset(self):
        """Full POR (platform reset release): debug port, core, volatile trims and the trim lock."""
        self.trim_locked = False
        self.trim = dict(TRIM_DEFAULTS)
        self.swd = SwdTarget(self)
        self.c_debugen = False
        self.vc_corereset = False
        self._reset_core()

    def fmax(self, vih):
        """Highest SWCLK frequency the chip follows at this VDD and tester drive level."""
        if self.vdd < 1.6 or not self.enabled:
            return 0.0
        base = 32e6 * min(1.0, (self.vdd - 1.0) / 2.3)
        margin = vih - 0.7 * self.vdd
        if margin <= 0:
            return 0.0
        return base * min(1.0, margin / 0.6)

    def trim_vref(self):
        """Bandgap output for the BG trim code (for trim loops on the analog side)."""
        return 1.2 + (self.trim[0x0] - 16) * 0.0025 + 0.011
