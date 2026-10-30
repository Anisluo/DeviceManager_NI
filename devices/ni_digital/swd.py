"""ARM Serial Wire Debug (ADIv5) host engine.

Protocol logic only. The bit-level transport is a *PHY* with one method::

    phy.run(cycles) -> list[int]     # bits sampled on the "r" cycles, in order

where each cycle is 'w0' / 'w1' (host drives SWDIO low/high), 'r' (host
released, target drives, sample) or 't' (turnaround: nobody drives, ignored).
Two PHYs exist on the 6571: static bit-bang (write_static/read_static, no
pattern compiler needed — use for first silicon) and pattern-based (source /
capture waveforms, full speed). Both the simulator and the real instrument
run exactly this code.
"""

# ACK values (as received LSB first: bit0 = first ack bit)
ACK_OK, ACK_WAIT, ACK_FAULT = 0b001, 0b010, 0b100

# DP registers (A[3:2] << 2)
DP_IDCODE, DP_ABORT, DP_CTRL_STAT, DP_SELECT, DP_RESEND, DP_RDBUFF = 0x0, 0x0, 0x4, 0x8, 0x8, 0xC
# MEM-AP registers
AP_CSW, AP_TAR, AP_DRW, AP_BASE, AP_IDR = 0x00, 0x04, 0x0C, 0xF8, 0xFC

CSW_WORD_INC = 0x23000012       # 32-bit access, auto-increment single, DbgSwEnable/HPROT as for Cortex-M
DHCSR, DCRSR, DCRDR, DEMCR, AIRCR, CPUID = 0xE000EDF0, 0xE000EDF4, 0xE000EDF8, 0xE000EDFC, 0xE000ED0C, 0xE000ED00
DBGKEY = 0xA05F0000

CORE_REGS = {f"r{i}": i for i in range(13)}
CORE_REGS.update({"sp": 13, "lr": 14, "pc": 15, "xpsr": 16, "msp": 17, "psp": 18})


class SwdError(Exception):
    def __init__(self, message, ack=None):
        super().__init__(message)
        self.ack = ack


def parity(value):
    return bin(value & 0xFFFFFFFF).count("1") & 1


def bits_lsb(value, n):
    return [(value >> i) & 1 for i in range(n)]


def from_bits(bits):
    return sum(b << i for i, b in enumerate(bits))


def request_bits(apndp, rnw, addr):
    a2, a3 = (addr >> 2) & 1, (addr >> 3) & 1
    p = (apndp + rnw + a2 + a3) & 1
    return [1, apndp, rnw, a2, a3, p, 0, 1]      # start, APnDP, RnW, A2, A3, parity, stop, park


def w(bits):
    return ["w1" if b else "w0" for b in bits]


class Swd:
    def __init__(self, phy, retries=100):
        self.phy = phy
        self.retries = retries
        self.select = None            # cached DP SELECT value
        self.csw = None
        self.transactions = 0

    # ---- low level -------------------------------------------------------
    def line_reset(self, jtag_to_swd=True):
        seq = w([1] * 56)
        if jtag_to_swd:
            seq += w(bits_lsb(0xE79E, 16)) + w([1] * 56)
        seq += w([0, 0, 0, 0])
        self.phy.run(seq)
        self.select = None
        self.csw = None

    def _transfer(self, apndp, rnw, addr, data=0):
        for _ in range(self.retries):
            self.transactions += 1
            head = w(request_bits(apndp, rnw, addr)) + ["t", "r", "r", "r"]
            if not rnw:
                head += ["t"]
            ack = from_bits(self.phy.run(head))
            if ack == ACK_OK:
                if rnw:
                    bits = self.phy.run(["r"] * 33 + ["t", "w0", "w0"])
                    value = from_bits(bits[:32])
                    if bits[32] != parity(value):
                        raise SwdError(f"read parity error (data 0x{value:08X})")
                    return value
                self.phy.run(w(bits_lsb(data, 32) + [parity(data)]) + ["w0", "w0"])
                return None
            # WAIT / FAULT / no response: turn the line around and idle
            self.phy.run((["t"] if rnw else []) + ["w0", "w0"])
            if ack == ACK_WAIT:
                continue
            if ack == ACK_FAULT:
                self._clear_errors()
                raise SwdError(f"FAULT response ({'AP' if apndp else 'DP'} 0x{addr:02X})", ack)
            raise SwdError(f"no valid ACK (got 0b{ack:03b}) — target not responding", ack)
        raise SwdError("WAIT timeout", ACK_WAIT)

    def _clear_errors(self):
        try:
            self._transfer(0, 0, DP_ABORT, 0x1E)      # STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR
        except SwdError:
            pass

    # ---- DP / AP ---------------------------------------------------------
    def read_dp(self, addr):
        return self._transfer(0, 1, addr)

    def write_dp(self, addr, value):
        self._transfer(0, 0, addr, value)

    def _select(self, apsel, addr):
        sel = (apsel << 24) | (addr & 0xF0)
        if sel != self.select:
            self.write_dp(DP_SELECT, sel)
            self.select = sel

    def read_ap(self, addr, apsel=0):
        self._select(apsel, addr)
        self._transfer(1, 1, addr & 0x0C)           # posted read; the value arrives via RDBUFF
        return self.read_dp(DP_RDBUFF)

    def write_ap(self, addr, value, apsel=0):
        self._select(apsel, addr)
        self._transfer(1, 0, addr & 0x0C, value)

    # ---- session ---------------------------------------------------------
    def connect(self):
        """Line reset, read IDCODE, power up the debug domain. Returns IDCODE."""
        self.line_reset()
        idcode = self.read_dp(DP_IDCODE)
        self.write_dp(DP_ABORT, 0x1E)
        self.write_dp(DP_CTRL_STAT, 0x50000000)     # CSYSPWRUPREQ | CDBGPWRUPREQ
        for _ in range(100):
            if (self.read_dp(DP_CTRL_STAT) & 0xA0000000) == 0xA0000000:
                break
        else:
            raise SwdError("debug power-up not acknowledged")
        return idcode

    # ---- memory ----------------------------------------------------------
    def _check_sticky(self, what):
        """AHB errors only set CTRL/STAT.STICKYERR (RDBUFF still ACKs OK): check after each access burst."""
        stat = self.read_dp(DP_CTRL_STAT)
        if stat & 0x20:
            self.write_dp(DP_ABORT, 0x1E)
            raise SwdError(f"bus error during {what} (CTRL/STAT 0x{stat:08X})")

    def _csw(self):
        if self.csw != CSW_WORD_INC:
            self.write_ap(AP_CSW, CSW_WORD_INC)
            self.csw = CSW_WORD_INC

    def read32(self, addr, count=1):
        if addr & 3:
            raise ValueError("address must be word aligned")
        self._csw()
        out = []
        for i in range(count):
            a = addr + 4 * i
            if i == 0 or (a & 0x3FF) == 0:          # TAR auto-increment wraps at 1 KB
                self.write_ap(AP_TAR, a)
            out.append(self.read_ap(AP_DRW))
        self._check_sticky(f"read @0x{addr:08X}")
        return out

    def write32(self, addr, values):
        if addr & 3:
            raise ValueError("address must be word aligned")
        self._csw()
        for i, v in enumerate(values):
            a = addr + 4 * i
            if i == 0 or (a & 0x3FF) == 0:
                self.write_ap(AP_TAR, a)
            self.write_ap(AP_DRW, v & 0xFFFFFFFF)
        self._check_sticky(f"write @0x{addr:08X}")

    # ---- core control (Cortex-M) -----------------------------------------
    def halt(self):
        self.write32(DHCSR, [DBGKEY | 0x3])
        return self.core_status()

    def resume(self):
        self.write32(DHCSR, [DBGKEY | 0x1])
        return self.core_status()

    def reset(self, halt=True):
        if halt:
            self.write32(DHCSR, [DBGKEY | 0x1])
            self.write32(DEMCR, [0x1])                # VC_CORERESET: stop at the reset vector
        self.write32(AIRCR, [0x05FA0004])             # SYSRESETREQ
        return self.core_status()

    def core_status(self):
        d = self.read32(DHCSR)[0]
        return {"dhcsr": d, "halted": bool(d & (1 << 17)), "debug_enabled": bool(d & 1),
                "lockup": bool(d & (1 << 19)), "sleeping": bool(d & (1 << 18)), "reset_seen": bool(d & (1 << 25))}

    def read_core_reg(self, name):
        sel = CORE_REGS[name.lower()] if isinstance(name, str) else int(name)
        self.write32(DCRSR, [sel])
        for _ in range(50):
            if self.read32(DHCSR)[0] & (1 << 16):    # S_REGRDY
                return self.read32(DCRDR)[0]
        raise SwdError("core register read timeout (is the core halted?)")

    def write_core_reg(self, name, value):
        sel = CORE_REGS[name.lower()] if isinstance(name, str) else int(name)
        self.write32(DCRDR, [value])
        self.write32(DCRSR, [sel | (1 << 16)])
        for _ in range(50):
            if self.read32(DHCSR)[0] & (1 << 16):
                return
        raise SwdError("core register write timeout (is the core halted?)")
