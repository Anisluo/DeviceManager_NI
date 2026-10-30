"""PXIe-6571 digital pattern instrument plugin.

Backends: "driver" (nimi-python nidigital on the PXIe-8881; with simulate:true the
driver simulates the instrument) and "sim" (pure Python 6571 + virtual Cortex-M0
chip, for development and CI). "auto" uses the driver when it can be opened.

Everything above the backend — SWD protocol, continuity / leakage algorithms,
result formatting — is shared, so the simulator exercises the same code paths
that run on the bench.
"""

import time

from dm.plugin import Plugin, PluginError, method, E_HW, E_STATE

# Helper modules must be imported here, at load time: the host puts the plugin folder on
# sys.path only while it loads this file. (driver_backend imports nidigital lazily.)
import swd as swdlib
from driver_backend import DriverBackend
from sim_backend import PPMU_RANGES, SimBackend

DEFAULT_PINS = {"SWCLK": 0, "SWDIO": 1, "nRST": 2, "PA0": 4, "PA1": 5, "PA2": 6, "PA3": 7,
                "SPI_SCK": 8, "SPI_MOSI": 9, "SPI_MISO": 10, "SPI_CS": 11}
DEFAULT_GROUPS = {"SWD": ["SWCLK", "SWDIO"], "GPIO": ["PA0", "PA1", "PA2", "PA3"],
                  "SPI": ["SPI_SCK", "SPI_MOSI", "SPI_MISO", "SPI_CS"]}
ARM_DESIGNERS = {0x23B: "ARM"}


def _int(v):
    """Accept 0x... strings, numbers, or floats from JSON."""
    if isinstance(v, str):
        return int(v.replace("_", ""), 0)
    return int(round(v))


def _hex(v):
    return f"0x{v & 0xFFFFFFFF:08X}"


class StaticPhy:
    """SWD by write_static / read_static: slow (~kHz) but needs no pattern compiler.
    Use for first silicon bring-up and to debug the pattern PHY."""
    name = "static"

    def __init__(self, backend):
        self.b = backend
        self.io = None

    def start(self):
        self.b.select_digital(["SWCLK", "SWDIO"])
        self.b.write_static(["SWCLK"], "0")
        self.io = None

    def run(self, cycles):
        captured = []
        for c in cycles:
            want = c[1] if c[0] == "w" else "X"
            if want != self.io:
                self.b.write_static(["SWDIO"], want)
                self.io = want
            self.b.write_static(["SWCLK"], "1")
            if c == "r":
                captured.append(1 if self.b.read_static(["SWDIO"])[0] == "H" else 0)
            self.b.write_static(["SWCLK"], "0")
        return captured


class PatternPhy:
    """SWD through source/capture patterns at the configured SWCLK frequency."""
    name = "pattern"

    def __init__(self, backend, frequency):
        self.b = backend
        self.frequency = frequency

    def start(self):
        pass

    def run(self, cycles):
        return self.b.swd_cycles(cycles, self.frequency)


class NiDigital(Plugin):
    def __init__(self, config):
        super().__init__(config)
        o = self.options
        # The host passes options through a JSON object that may reorder keys: list pins by channel.
        self.pin_map = dict(sorted(((k, int(v)) for k, v in (o.get("pins") or DEFAULT_PINS).items()), key=lambda kv: kv[1]))
        self.groups = o.get("groups") or {k: [p for p in v if p in self.pin_map] for k, v in DEFAULT_GROUPS.items()}
        self.pulls = o.get("pulls") or {"SWDIO": "up", "nRST": "up", "SPI_CS": "up", "SWCLK": "down"}
        self.vdd_nominal = float(o.get("vdd", 3.3))
        self.expect_idcode = _int(o.get("expect_idcode", "0x0BB11477"))   # pass criterion of swd_check
        kind = o.get("backend", "auto")
        self.backend = None
        if kind in ("driver", "auto"):
            try:
                self.backend = DriverBackend(self.resource or "PXI1Slot15", self.pin_map,
                                             dict(o, simulate=self.config.get("simulate", False)))
            except Exception as e:  # noqa: BLE001
                if kind == "driver":
                    raise
                self.log(f"NI-Digital 不可用（{e}），改用纯仿真后端", "warning")
        if self.backend is None:
            self.backend = SimBackend(self.resource or "SIM6571", self.pin_map, o.get("sim", {}))
        self.swd_phy_name = o.get("swd_phy", "static")
        self.swd_freq = float(o.get("swd_frequency", 1e6))
        self.swd = None
        self.connected = False
        self._apply_default_levels()
        self.log(f"{self.backend.info()['model']} · {len(self.pin_map)} 个引脚 · 后端 {self.backend.name}")

    def close(self):
        self.backend.close()

    # ------------------------------------------------------------------ helpers
    def _pins(self, pins):
        if pins is None or pins == "all":
            return list(self.pin_map)
        if isinstance(pins, str):
            out = []
            for item in [p.strip() for p in pins.split(",") if p.strip()]:
                out += self.groups.get(item, [item])
            pins = out
        for p in pins:
            if p not in self.pin_map:
                raise ValueError(f"unknown pin {p!r} (pins: {', '.join(self.pin_map)})")
        return list(dict.fromkeys(pins))

    def _apply_default_levels(self):
        v = self.vdd_nominal
        self.backend.set_levels(list(self.pin_map), vil=0.0, vih=v, vol=0.3 * v, voh=0.7 * v, vterm=v / 2)

    def _swd(self):
        if self.swd is None:
            phy = StaticPhy(self.backend) if self.swd_phy_name == "static" else PatternPhy(self.backend, self.swd_freq)
            phy.start()
            self.swd = swdlib.Swd(phy)
        return self.swd

    def _swd_call(self, fn):
        try:
            return fn(self._swd())
        except swdlib.SwdError as e:
            self.connected = False
            raise PluginError(f"SWD: {e}", E_HW)

    # ------------------------------------------------------------------ info / pins
    @method("Instrument, backend and pin configuration")
    def get_info(self):
        info = self.backend.info()
        info.update({"pins": self.pin_map, "groups": self.groups, "swd_phy": self.swd_phy_name,
                     "swd_frequency": self.swd_freq, "connected": self.connected})
        return info

    @method("Pin names and groups (for page selectors)")
    def list_pins(self):
        return {"pins": list(self.pin_map), "groups": list(self.groups),
                "choices": ["all"] + list(self.groups) + list(self.pin_map)}

    @method("Per-pin channel, levels and static state")
    def pin_table(self):
        states = self.backend.read_static(list(self.pin_map))
        rows = []
        for (p, ch), st in zip(self.pin_map.items(), states):
            lv = self.backend.get_levels(p)
            rows.append({"pin": p, "channel": ch, "state": st, "pull": self.pulls.get(p, ""), **lv})
        return {"rows": rows}

    @method("Reset levels to the nominal VDD, PPMU off, drivers off")
    def reset(self):
        pins = list(self.pin_map)
        self.backend.ppmu_off(pins)
        self.backend.select_digital(pins)
        self.backend.write_static(pins, "X")
        self._apply_default_levels()
        self.swd = None
        self.connected = False

    # ------------------------------------------------------------------ levels / timing / static
    @method("Set voltage levels on pins (a group, a list or 'all')")
    def set_levels(self, pins: str = "all", vil: float = None, vih: float = None, vol: float = None,
                   voh: float = None, vterm: float = None):
        self.backend.set_levels(self._pins(pins), vil, vih, vol, voh, vterm)
        return self.get_levels(self._pins(pins)[0])

    @method("Levels of one pin")
    def get_levels(self, pin: str = "SWDIO"):
        return dict(self.backend.get_levels(pin), pin=pin)

    @method("Pattern period (s)")
    def set_period(self, period: float):
        self.backend.set_period(float(period))
        return {"period": float(period), "frequency": 1.0 / float(period)}

    @method("Drive pins statically: state 0 | 1 | X")
    def write_static(self, pins: str, state: str):
        p = self._pins(pins)
        self.backend.select_digital(p)
        self.backend.write_static(p, state)
        self.swd = None

    @method("Read the comparators: L | H | M per pin")
    def read_static(self, pins: str = "all"):
        p = self._pins(pins)
        return {"states": dict(zip(p, self.backend.read_static(p)))}

    # ------------------------------------------------------------------ PPMU
    def _range_for(self, current):
        a = abs(current)
        for r in PPMU_RANGES:
            if a <= r:
                return r
        raise ValueError(f"{current} A exceeds the largest PPMU range (32 mA)")

    @method("PPMU force voltage, with a current range / clamp")
    def ppmu_force_voltage(self, pins: str, voltage: float, current_range: float = 2e-3):
        p = self._pins(pins)
        self.backend.ppmu_configure(p, "voltage", float(voltage), float(current_range))
        self.swd = None
        return self.ppmu_measure(pins, "both")

    @method("PPMU force current with voltage limits")
    def ppmu_force_current(self, pins: str, current: float, current_range: float = None,
                           v_low: float = -2.0, v_high: float = 6.0):
        p = self._pins(pins)
        rng = float(current_range) if current_range else self._range_for(current)
        self.backend.ppmu_configure(p, "current", float(current), rng, float(v_low), float(v_high))
        self.swd = None
        return self.ppmu_measure(pins, "both")

    @method("PPMU measure: current | voltage | both")
    def ppmu_measure(self, pins: str = "all", what: str = "both"):
        p = self._pins(pins)
        rows = [{"pin": x} for x in p]
        if what in ("voltage", "both"):
            for r, v in zip(rows, self.backend.ppmu_measure(p, "voltage")):
                r["voltage"] = v
        if what in ("current", "both"):
            for r, v in zip(rows, self.backend.ppmu_measure(p, "current")):
                r["current"] = v
        out = {"rows": rows}
        if len(rows) == 1:
            out.update({k: v for k, v in rows[0].items() if k != "pin"})
        return out

    @method("PPMU off (disconnect) and back to digital")
    def ppmu_off(self, pins: str = "all"):
        p = self._pins(pins)
        self.backend.ppmu_off(p)
        self.backend.select_digital(p)

    # ------------------------------------------------------------------ open / short / leakage
    @method("Open/short test: force a small negative current per pin, the others held at 0 V, "
            "measure the ESD-diode voltage")
    def continuity(self, pins: str = "all", current: float = -100e-6, low: float = -0.95, high: float = -0.25,
                   clamp: float = -1.5, sequential: bool = True):
        p = self._pins(pins)
        all_pins = list(self.pin_map)
        rng = self._range_for(current)
        rows = []
        t0 = time.time()
        try:
            groups = [[x] for x in p] if sequential else [p]
            for g in groups:
                others = [x for x in all_pins if x not in g]
                if others:
                    self.backend.ppmu_configure(others, "voltage", 0.0, 2e-3)
                self.backend.ppmu_configure(g, "current", float(current), rng, v_low=clamp, v_high=1.0)
                for pin, v in zip(g, self.backend.ppmu_measure(g, "voltage")):
                    if v <= clamp + 0.05:
                        status = "open"
                    elif v > high:
                        status = "short"
                    elif v < low:
                        status = "open"
                    else:
                        status = "ok"
                    rows.append({"pin": pin, "voltage": v, "status": status})
        finally:
            self.backend.ppmu_off(all_pins)
            self.backend.select_digital(all_pins)
            self.swd = None
        fails = [r["pin"] for r in rows if r["status"] != "ok"]
        self.emit({"type": "state", "continuity": "pass" if not fails else "fail", "fails": fails})
        return {"pass": not fails, "fails": fails, "rows": rows, "limits": [low, high], "current": current,
                "time_s": time.time() - t0}

    @method("Leakage: force VIL and VIH levels per pin with the PPMU, measure the current")
    def leakage(self, pins: str = "all", v_low: float = 0.0, v_high: float = None, limit: float = 1e-6,
                current_range: float = 32e-6):
        p = self._pins(pins)
        v_high = self.vdd_nominal if v_high is None else float(v_high)
        rows = {x: {"pin": x, "pull": self.pulls.get(x, "")} for x in p}
        try:
            for key, v in (("i_low", v_low), ("i_high", v_high)):
                self.backend.ppmu_configure(p, "voltage", float(v), float(current_range))
                for pin, i in zip(p, self.backend.ppmu_measure(p, "current")):
                    rows[pin][key] = i
                    if abs(i) >= 0.98 * float(current_range):
                        rows[pin]["clamped"] = True       # at the range limit: true current is larger
        finally:
            self.backend.ppmu_off(p)
            self.backend.select_digital(p)
            self.swd = None
        out = []
        for r in rows.values():
            # A pull-up legitimately sinks current at VIL (pull-down at VIH): judge only the other side.
            check = [r["i_high"]] if r["pull"] == "up" else [r["i_low"]] if r["pull"] == "down" else [r["i_low"], r["i_high"]]
            r["status"] = "ok" if all(abs(i) <= limit for i in check) else "fail"
            out.append(r)
        fails = [r["pin"] for r in out if r["status"] != "ok"]
        return {"pass": not fails, "fails": fails, "rows": out, "limit": limit, "v_low": v_low, "v_high": v_high}

    # ------------------------------------------------------------------ SWD
    @method("SWD PHY (static | pattern) and SWCLK frequency for the pattern PHY")
    def swd_configure(self, phy: str = None, frequency: float = None):
        if phy is not None:
            if phy not in ("static", "pattern"):
                raise ValueError("phy must be static or pattern")
            self.swd_phy_name = phy
        if frequency is not None:
            if not 1e3 <= float(frequency) <= 50e6:
                raise ValueError("SWCLK frequency must be within 1 kHz … 50 MHz")
            self.swd_freq = float(frequency)
        self.swd = None
        self.connected = False
        return {"phy": self.swd_phy_name, "frequency": self.swd_freq}

    @method("Line reset, IDCODE, debug power-up; reads CPUID, chip ID and AP IDR")
    def swd_connect(self):
        t0 = time.time()

        def go(s):
            idcode = s.connect()
            ap_idr = s.read_ap(swdlib.AP_IDR)
            cpuid = s.read32(swdlib.CPUID)[0]
            try:
                chip_id = s.read32(0x40000000)[0]
            except swdlib.SwdError:
                chip_id = None
            return idcode, ap_idr, cpuid, chip_id

        idcode, ap_idr, cpuid, chip_id = self._swd_call(go)
        self.connected = True
        designer = (idcode >> 1) & 0x7FF
        res = {"connected": True, "idcode": idcode, "idcode_hex": _hex(idcode),
               "designer": ARM_DESIGNERS.get(designer, f"0x{designer:03X}"), "partno": (idcode >> 12) & 0xFFFF,
               "version": idcode >> 28, "ap_idr_hex": _hex(ap_idr), "cpuid_hex": _hex(cpuid),
               "core": "Cortex-M0" if (cpuid >> 4) & 0xFFF == 0xC20 else f"partno 0x{(cpuid >> 4) & 0xFFF:03X}",
               "chip_id_hex": _hex(chip_id) if chip_id is not None else "—",
               "phy": self.swd_phy_name, "frequency": self.swd_freq,
               "transactions": self.swd.transactions, "time_ms": (time.time() - t0) * 1e3}
        self.emit({"type": "state", "swd": "connected", "idcode": res["idcode_hex"]})
        return res

    @method("Pass/fail SWD check (line reset + IDCODE == expected) — for shmoos")
    def swd_check(self, expect_idcode: str = None):
        self.swd = None
        try:
            s = self._swd()
            s.line_reset()
            idcode = s.read_dp(swdlib.DP_IDCODE)
        except swdlib.SwdError as e:
            return {"pass": False, "error": str(e)}
        # Compare with the expected value: near a level threshold random bits can still form a "valid" word.
        ok = idcode == (_int(expect_idcode) if expect_idcode else self.expect_idcode)
        return {"pass": bool(ok), "idcode_hex": _hex(idcode)}

    @method("Read a DP register (0x0 IDCODE, 0x4 CTRL/STAT, 0xC RDBUFF)")
    def swd_read_dp(self, address: str = "0x0"):
        v = self._swd_call(lambda s: s.read_dp(_int(address)))
        return {"value": v, "hex": _hex(v)}

    @method("Write a DP register")
    def swd_write_dp(self, address: str, value: str):
        self._swd_call(lambda s: s.write_dp(_int(address), _int(value)))

    @method("Read an AP register (AP #apsel)")
    def swd_read_ap(self, address: str = "0xFC", apsel: int = 0):
        v = self._swd_call(lambda s: s.read_ap(_int(address), apsel))
        return {"value": v, "hex": _hex(v)}

    @method("Write an AP register")
    def swd_write_ap(self, address: str, value: str, apsel: int = 0):
        self._swd_call(lambda s: s.write_ap(_int(address), _int(value), apsel))

    def _need_connection(self):
        if not self.connected:
            self.swd_connect()

    @method("Read 32-bit words through the AHB-AP")
    def mem_read(self, address: str = "0x20000000", count: int = 8):
        self._need_connection()
        a = _int(address)
        count = int(count)
        if not 1 <= count <= 4096:
            raise ValueError("count must be within 1 … 4096")
        words = self._swd_call(lambda s: s.read32(a, count))
        return {"address": a, "words": words,
                "rows": [{"address": _hex(a + 4 * i), "value": _hex(v), "dec": v} for i, v in enumerate(words)]}

    @method("Write 32-bit word(s) through the AHB-AP; value may be a list")
    def mem_write(self, address: str, value=None, values: list = None, verify: bool = True):
        self._need_connection()
        a = _int(address)
        data = [_int(v) for v in (values if values is not None else [value])]
        self._swd_call(lambda s: s.write32(a, data))
        if verify:
            back = self._swd_call(lambda s: s.read32(a, len(data)))
            mismatches = [i for i, (x, y) in enumerate(zip(data, back)) if x != y]
            return {"written": len(data), "verified": not mismatches, "mismatch_at": [_hex(a + 4 * i) for i in mismatches]}
        return {"written": len(data)}

    @method("Halt the core")
    def core_halt(self):
        self._need_connection()
        return self._swd_call(lambda s: s.halt())

    @method("Resume the core")
    def core_resume(self):
        self._need_connection()
        return self._swd_call(lambda s: s.resume())

    @method("System reset (optionally halting at the reset vector)")
    def core_reset(self, halt: bool = True):
        self._need_connection()
        return self._swd_call(lambda s: s.reset(bool(halt)))

    @method("Core status (DHCSR)")
    def core_status(self):
        self._need_connection()
        return self._swd_call(lambda s: s.core_status())

    @method("Core registers r0–r12, sp, lr, pc, xpsr (core must be halted)")
    def core_regs(self):
        self._need_connection()

        def go(s):
            if not s.core_status()["halted"]:
                raise PluginError("core is running — halt it first", E_STATE)
            return [{"reg": n, "value": _hex(v), "dec": v} for n, v in
                    ((n, s.read_core_reg(n)) for n in list(swdlib.CORE_REGS)[:17])]
        return {"rows": self._swd_call(go)}

    @method("Write a core register (core must be halted)")
    def core_write_reg(self, reg: str, value: str):
        self._need_connection()
        self._swd_call(lambda s: s.write_core_reg(reg, _int(value)))

    # ------------------------------------------------------------------ patterns
    @method("Load a pattern project folder (pinmap, specs, levels, timing, digipat)")
    def load_project(self, folder: str):
        return self.backend.load_project(folder)

    @method("Load one compiled pattern (.digipat)")
    def load_pattern(self, path: str):
        return {"loaded": self.backend.load_pattern(path)}

    @method("Burst a pattern from a start label; returns pass/fail and per-pin fail counts")
    def burst(self, start_label: str, timeout: float = 10.0):
        r = self.backend.burst(start_label, float(timeout))
        r["rows"] = [{"pin": p, "fails": n, "status": "ok" if n == 0 else "fail"} for p, n in r["fail_count"].items()]
        r["result"] = "PASS" if r["pass"] else "FAIL"
        self.swd = None
        return r

    # ------------------------------------------------------------------ project patterns
    # For test tasks (e.g. TDC测量), called through the host: load a Digital Pattern Editor
    # project, burst one pattern at a time without waiting, poll, abort. Loading a project
    # replaces this instrument's pin map, so the SWD functions need a reset afterwards.
    @method("Load a pattern project: pin map, specifications, levels and timing sheets (file paths)")
    def pattern_project(self, pinmap: str, specs: list = None, levels: list = None, timing: list = None):
        self.swd = None
        return self.backend.project_open(pinmap, specs or [], levels or [], timing or [])

    @method("Apply a timing sheet (loading it first if needed)")
    def pattern_timing(self, path: str):
        return self.backend.use_timing(path)

    @method("Replace the specifications file and re-apply levels and timing")
    def pattern_specs(self, path: str):
        return self.backend.set_specs(path)

    @method("Burst one pattern without waiting; sim_seconds is the run time of a simulated burst")
    def pattern_start(self, path: str, label: str, sim_seconds: float = 0.0):
        self.backend.burst_start(path, label, float(sim_seconds))
        self.burst_pins = None
        return {"started": label}

    @method("Burst state: done, pass/fail and per-pin fail counts once done")
    def pattern_status(self, pins: list = None):
        done = self.backend.burst_done()
        out = {"done": done}
        if done:
            out["pass"], out["fail_count"] = self.backend.burst_result(pins or [])
        return out

    @method("Abort a running burst")
    def pattern_abort(self):
        self.backend.burst_abort()
        return {"aborted": True}

    @method("Loaded pattern names")
    def list_patterns(self):
        return {"patterns": sorted(getattr(self.backend, "patterns", {}) or [])}

    # ------------------------------------------------------------------ simulator controls
    def _sim(self):
        if self.backend.name != "sim":
            raise PluginError("only available on the simulated backend", E_STATE)
        return self.backend

    @method("[sim] Inject a pin fault: open | short_gnd | short_vdd | short:<pin> | leaky | none")
    def sim_inject_fault(self, pin: str, fault: str = "none"):
        self._sim().inject_fault(pin, None if fault in ("none", "", None) else fault)
        self.swd = None
        self.connected = False

    @method("[sim] Set the virtual chip's VDD")
    def sim_set_vdd(self, vdd: float):
        self._sim().chip.vdd = float(vdd)
        self.swd = None
        self.connected = False
        return {"vdd": float(vdd)}

    @method("[sim] Bandgap voltage produced by the current TRIM_BG code")
    def sim_vref(self):
        chip = self._sim().chip
        return {"vref": chip.trim_vref(), "trim_bg": chip.trim[0x0]}
