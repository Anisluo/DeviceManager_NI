"""XC7K480T "virtual chip" emulation platform (pre-silicon).

The chip's digital RTL runs on a Kintex-7 board; the PXIe-6571 talks to it over
SWD/SPI/GPIO at 1.8 V exactly as it will to silicon. This plugin is the control
plane next to that:

* JTAG (Vivado / Vivado Lab hw_server): program bitstreams, configuration status,
  XADC temperature and supplies, JTAG-to-AXI register access as a fallback.
* UART register bridge (rtl/dm_uart_bridge.v): platform registers — DUT reset and
  clock, mode straps, SWCLK activity from the 6571, trim mirrors, probes.
* Analog stubs: the RTL has no analog; the host turns the trim codes the chip
  holds into VREF / LDO / oscillator values with a configurable model, so trim
  loops and datasheet flows run before tape-out.

Backends: "hw" (pyserial + Vivado) or "sim" (board model sharing a virtual chip
with the 6571 simulator, see station sim_fpga.json).
"""

import glob
import json
import os
import time

from dm.plugin import Plugin, PluginError, method, E_HW, E_STATE

# Imported at load time (the plugin folder is on sys.path only then).
from sim_board import SimBoard
from uart_link import LinkError, SerialTransport, UartLink
from vivado import HwSession, SimHw, TclError, Vivado, find_vivado

HERE = os.path.dirname(os.path.abspath(__file__))
SYS_CLK = 200e6

DEFAULT_ANALOG = {
    "vref":  {"trim": "TRIM_BG",  "center": 16, "step": 0.0025, "nominal": 1.200, "offset": 0.011, "unit": "V"},
    "vldo":  {"trim": "TRIM_LDO", "center": 16, "step": 0.0125, "nominal": 1.200, "offset": -0.018, "unit": "V"},
    "fosc":  {"trim": "TRIM_OSC", "center": 64, "step": 0.0035e6, "nominal": 8.0e6, "offset": 0.21e6, "unit": "Hz"},
}


def _int(v):
    return int(v.replace("_", ""), 0) if isinstance(v, str) else int(round(v))


def _hex(v):
    return f"0x{v & 0xFFFFFFFF:08X}"


class FpgaK7(Plugin):
    def __init__(self, config):
        super().__init__(config)
        o = self.options
        with open(os.path.join(HERE, "regmap.json"), encoding="utf-8") as fh:
            rm = json.load(fh)
        self.regs = {r["name"]: r for r in rm["registers"]}
        self.addr_name = {int(r["address"], 16): r["name"] for r in rm["registers"]}
        self.analog = {k: dict(DEFAULT_ANALOG[k], **(o.get("analog", {}).get(k, {}))) for k in DEFAULT_ANALOG}
        self.bitstream_dir = o.get("bitstream_dir") or os.path.join(HERE, "bitstreams")
        self.backend = o.get("backend", "sim" if self.simulate else "hw")
        self.hw = None             # HwSession / SimHw, opened on first JTAG use
        self.vivado = None

        if self.backend == "sim":
            self.board = SimBoard(o.get("sim", {}))
            self.link = UartLink(self.board, timeout=0.3)
            self.access = "uart"
        else:
            self.board = None
            self.access = o.get("reg_access", "uart" if o.get("uart_port") else "jtag")
            self.link = None
            if self.access == "uart":
                self.link = UartLink(SerialTransport(o["uart_port"], int(o.get("baud", 921600))),
                                     timeout=float(o.get("uart_timeout", 0.5)))
        self.log(f"K7 平台 · 后端 {self.backend} · 寄存器访问 {self.access}"
                 + (f" · {self.link.t.name}" if self.link else ""))

    def close(self):
        if self.link is not None and self.backend != "sim":
            self.link.t.close()
        if self.vivado is not None:
            self.vivado.close()

    # ------------------------------------------------------------------ register access
    def _addr(self, a):
        if isinstance(a, str) and a in self.regs:
            return int(self.regs[a]["address"], 16)
        return _int(a)

    def _rd(self, addr, count=1):
        try:
            if self.access == "uart":
                return self.link.read(addr, count)
            hw = self._jtag()
            return [hw.axi_read(addr + 4 * i) for i in range(count)]
        except (LinkError, TclError) as e:
            raise PluginError(f"读寄存器 {_hex(addr)} 失败：{e}", E_HW)

    def _wr(self, addr, value):
        try:
            if self.access == "uart":
                self.link.write(addr, value)
            else:
                self._jtag().axi_write(addr, value)
        except (LinkError, TclError) as e:
            raise PluginError(f"写寄存器 {_hex(addr)} 失败：{e}", E_HW)

    def _reg(self, name):
        return self._rd(self._addr(name))[0]

    # ------------------------------------------------------------------ JTAG
    def _jtag(self):
        if self.hw is None:
            if self.backend == "sim":
                self.hw = SimHw(self.board)
            else:
                try:
                    self.vivado = Vivado(self.options.get("vivado"), log=lambda l: None)
                    self.hw = HwSession(self.vivado, self.options.get("hw_server", "localhost:3121"),
                                        self.options.get("jtag_target", ""))
                except (TclError, RuntimeError, OSError) as e:
                    self.vivado = None
                    raise PluginError(f"JTAG / Vivado：{e}", E_HW)
        return self.hw

    @method("Open the JTAG target (starts Vivado on first use) and read the device info")
    def jtag_connect(self):
        t0 = time.time()
        info = self._jtag().info()
        info["time_s"] = time.time() - t0
        info["vivado"] = getattr(self.vivado, "exe", "simulated") if self.backend != "sim" else "simulated"
        self.emit({"type": "state", "jtag": "connected", "device": info.get("device")})
        return info

    @method("Bitstreams found in the bitstream folder")
    def list_bitstreams(self):
        files = sorted(glob.glob(os.path.join(self.bitstream_dir, "*.bit")), key=os.path.getmtime, reverse=True)
        if self.backend == "sim" and not files:
            files = [os.path.join(self.bitstream_dir, "virtual_chip_sim.bit")]
        rows = [{"file": os.path.basename(f), "path": f,
                 "time": time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(f))) if os.path.exists(f) else "—"}
                for f in files]
        return {"rows": rows, "names": [r["file"] for r in rows], "folder": self.bitstream_dir}

    @method("Program a bitstream over JTAG (name in the bitstream folder or full path); then checks the platform ID")
    def program(self, bitstream: str = "", probes: str = None):
        if not bitstream:
            names = self.list_bitstreams()["names"]
            if not names:
                raise ValueError(f"{self.bitstream_dir} 中没有 .bit 文件")
            bitstream = names[0]
        path = bitstream if os.path.isabs(bitstream) else os.path.join(self.bitstream_dir, bitstream)
        if self.backend != "sim" and not os.path.exists(path):
            raise ValueError(f"找不到 {path}")
        t0 = time.time()
        try:
            info = self._jtag().program(path, probes)
        except TclError as e:
            raise PluginError(f"下载失败：{e}", E_HW)
        info["bitstream"] = os.path.basename(path)
        info["program_s"] = time.time() - t0
        time.sleep(0.05)
        try:
            info.update(self.ping())
        except PluginError as e:
            info["platform"] = f"下载完成但寄存器无响应：{e}"
        self.log(f"已下载 {info['bitstream']}（{info['program_s']:.1f} s）")
        return info

    @method("XADC: die temperature and VCCINT / VCCAUX / VCCBRAM")
    def xadc(self):
        try:
            return self._jtag().xadc()
        except TclError as e:
            raise PluginError(f"XADC：{e}", E_HW)

    # ------------------------------------------------------------------ platform registers
    @method("Check the register link: ID must be 0x444D4B37; returns build id")
    def ping(self):
        t0 = time.time()
        ident, build = self._rd(0x000, 2)
        if ident != 0x444D4B37:
            raise PluginError(f"平台 ID {_hex(ident)}（期望 0x444D4B37）— bitstream 不含 DeviceManager 平台？", E_STATE)
        return {"ok": True, "id_hex": _hex(ident), "build_hex": _hex(build), "latency_ms": (time.time() - t0) * 1e3}

    @method("Decoded platform status (for the page)")
    def status(self):
        st = self._reg("STATUS")
        ctrl = self._reg("CTRL")
        div = self._reg("CLK_DIV")
        trims = self._rd(0x100, 3)
        return {"clk_locked": bool(st & 1), "dut_running": bool(st & 2), "swclk_active": bool(st & 4),
                "dut_reset": bool(ctrl & 1), "dut_clk_en": bool(ctrl & 2),
                "dut_clock_hz": SYS_CLK / (2 * (div + 1)), "clk_div": div,
                "uptime_s": self._reg("UPTIME_MS") / 1000.0, "swclk_edges": self._reg("SWCLK_EDGES"),
                "trim_bg": trims[0], "trim_ldo": trims[1], "trim_osc": trims[2],
                "gpio_out_hex": _hex(self._reg("GPIO_OUT")), "gpio_in_hex": _hex(self._reg("GPIO_IN"))}

    @method("All named registers with values")
    def reg_dump(self):
        rows = []
        for name, r in self.regs.items():
            a = int(r["address"], 16)
            v = self._rd(a)[0]
            rows.append({"name": name, "address": f"0x{a:03X}", "value": _hex(v), "dec": v,
                         "access": r["access"], "doc": r["doc"]})
        return {"rows": rows}

    @method("Read register(s) by name or address")
    def reg_read(self, address: str = "ID", count: int = 1):
        a = self._addr(address)
        words = self._rd(a, int(count))
        rows = [{"address": f"0x{a + 4 * i:03X}", "name": self.addr_name.get(a + 4 * i, ""), "value": _hex(v), "dec": v}
                for i, v in enumerate(words)]
        out = {"rows": rows, "words": words}
        if len(words) == 1:
            out.update({"value": words[0], "hex": _hex(words[0])})
        return out

    @method("Write a register by name or address")
    def reg_write(self, address: str, value: str):
        a = self._addr(address)
        r = self.regs.get(self.addr_name.get(a, ""), {})
        if r.get("access") == "ro":
            raise ValueError(f"{r['name']} 是只读寄存器")
        self._wr(a, _int(value))
        return {"address": f"0x{a:03X}", "readback": _hex(self._rd(a)[0])}

    @method("Hold or release the virtual chip in reset (release = power-on reset)")
    def dut_reset(self, hold: bool = True):
        ctrl = self._reg("CTRL")
        self._wr(self._addr("CTRL"), (ctrl & ~1) | int(bool(hold)))
        self.emit({"type": "state", "dut_reset": bool(hold)})
        return self.status()

    @method("DUT core clock: enable and/or frequency (sys_clk / (2·(div+1)), sys_clk = 200 MHz)")
    def dut_clock(self, enable: bool = None, mhz: float = None):
        if mhz is not None:
            div = max(0, min(255, round(SYS_CLK / (2 * float(mhz) * 1e6)) - 1))
            self._wr(self._addr("CLK_DIV"), div)
        if enable is not None:
            ctrl = self._reg("CTRL")
            self._wr(self._addr("CTRL"), (ctrl & ~2) | (2 if enable else 0))
        return self.status()

    @method("Mode straps to the chip (GPIO_OUT[7:0])")
    def set_mode(self, mode: str):
        v = _int(mode)
        g = self._reg("GPIO_OUT")
        self._wr(self._addr("GPIO_OUT"), (g & ~0xFF) | (v & 0xFF))
        return {"gpio_out_hex": _hex(self._reg("GPIO_OUT"))}

    @method("SWCLK edges counted since configuration (6571 → FPGA wiring and level check)")
    def swclk_edges(self):
        return {"edges": self._reg("SWCLK_EDGES")}

    # ------------------------------------------------------------------ analog stubs
    @method("Analog stub values computed from the chip's trim codes (VREF, VLDO, FOSC)")
    def analog_model(self):
        trims = dict(zip(("TRIM_BG", "TRIM_LDO", "TRIM_OSC"), self._rd(0x100, 3)))
        out, rows = {}, []
        for key, m in self.analog.items():
            code = trims[m["trim"]]
            value = m["nominal"] + m["offset"] + (code - m["center"]) * m["step"]
            out[key] = value
            out[key + "_code"] = code
            rows.append({"quantity": key, "trim": m["trim"], "code": code, "value": value, "unit": m["unit"],
                         "error_pct": (value / m["nominal"] - 1) * 100})
        out["rows"] = rows
        return out
