"""Simulated PXIe-6571: 32 channels, levels, static I/O, per-pin PPMU, pattern
bursts, and SWD over patterns — wired to the virtual chip in sim_dut.py."""

import os
import time
import random

from sim_dut import SimChip

PPMU_RANGES = [2e-6, 8e-6, 32e-6, 128e-6, 512e-6, 2e-3, 32e-3]
V_MIN, V_MAX = -2.0, 6.0


class Channel:
    def __init__(self, pin, channel):
        self.pin = pin
        self.channel = channel
        self.vil, self.vih, self.vol, self.voh, self.vterm = 0.0, 3.3, 1.0, 2.3, 1.65
        self.function = "digital"          # digital | ppmu | off
        self.static = "X"                  # 0 | 1 | X
        self.ppmu = {"mode": "voltage", "level": 0.0, "range": 2e-3, "v_low": V_MIN, "v_high": V_MAX, "sourcing": False}


class SimBackend:
    name = "sim"

    def __init__(self, resource, pins, options):
        self.resource = resource
        self.rng = random.Random(options.get("seed"))
        # "shared_dut": attach to a chip object other simulators also use (e.g. the FPGA platform sim)
        key = options.get("shared_dut")
        if key:
            import dm.shared
            self.chip = dm.shared.setdefault(key, lambda: SimChip(seed=options.get("seed")))
        else:
            self.chip = SimChip(seed=options.get("seed"))
        self.chip.vdd = float(options.get("vdd", 3.3))
        self.channels = {p: Channel(p, ch) for p, ch in pins.items()}
        self.period = 100e-9
        self.patterns = {}
        self.dut_drive = {}                # pin -> bit the chip drives on that pin
        for pin, fault in (options.get("faults") or {}).items():
            self.inject_fault(pin, fault)

    def info(self):
        return {"backend": "sim", "model": "PXIe-6571 (simulated)", "resource": self.resource,
                "channels": 32, "serial": "SIM6571", "dut": "virtual Cortex-M0 test chip", "vdd": self.chip.vdd}

    def close(self):
        pass

    # ---- helpers ----
    def _ch(self, pin):
        if pin not in self.channels:
            raise ValueError(f"unknown pin {pin!r}")
        return self.channels[pin]

    def _dut_pin(self, pin):
        return self.chip.pins.get(pin)

    def inject_fault(self, pin, fault):
        p = self._dut_pin(pin)
        if p is None:
            raise ValueError(f"the simulated chip has no pin {pin!r}")
        p.fault = fault or None

    # ---- levels & timing ----
    def set_levels(self, pins, vil=None, vih=None, vol=None, voh=None, vterm=None):
        for p in pins:
            c = self._ch(p)
            for k, v in (("vil", vil), ("vih", vih), ("vol", vol), ("voh", voh), ("vterm", vterm)):
                if v is not None:
                    if not V_MIN <= v <= V_MAX:
                        raise ValueError(f"{k} {v} V outside -2 … 6 V")
                    setattr(c, k, float(v))

    def get_levels(self, pin):
        c = self._ch(pin)
        return {"vil": c.vil, "vih": c.vih, "vol": c.vol, "voh": c.voh, "vterm": c.vterm}

    def set_period(self, period):
        if not 10e-9 <= period <= 40e-6:
            raise ValueError("period must be within 10 ns … 40 µs (6571: up to 100 MHz)")
        self.period = float(period)

    # ---- static digital ----
    def select_digital(self, pins):
        for p in pins:
            c = self._ch(p)
            c.function = "digital"
            c.ppmu["sourcing"] = False

    def _line_voltage(self, pin):
        c = self.channels[pin]
        dut = self._dut_pin(pin)
        if c.function == "ppmu" and c.ppmu["sourcing"]:
            return self._ppmu_voltage(pin)
        if c.function == "digital" and c.static in "01":
            return c.vih if c.static == "1" else c.vil
        if dut is None or dut.fault == "open":
            return c.vterm
        if pin in self.dut_drive and self.dut_drive[pin] is not None:
            return self.chip.vdd if self.dut_drive[pin] else 0.0
        if dut.fault == "short_gnd":
            return 0.0
        if dut.pull == "up":
            return self.chip.vdd
        if dut.pull == "down":
            return 0.0
        return c.vterm

    def _dut_sees(self, pin, voltage):
        """Logic level the chip's input buffer sees, or a random bit in the undefined band."""
        vdd = self.chip.vdd
        if voltage >= 0.7 * vdd:
            return 1
        if voltage <= 0.3 * vdd:
            return 0
        return self.rng.randint(0, 1)

    def write_static(self, pins, state):
        state = str(state).upper()
        if state not in ("0", "1", "X"):
            raise ValueError("state must be 0, 1 or X")
        for p in pins:
            c = self._ch(p)
            c.function = "digital"
            prev = c.static
            c.static = state
            if p == "SWCLK" and prev != "1" and state == "1" and c.vih >= 0.7 * self.chip.vdd and self.chip.vdd >= 1.6 and self.chip.enabled:
                self._swd_edge()

    def _swd_edge(self):
        if "SWDIO" not in self.channels:
            return
        io = self.channels["SWDIO"]
        line = None
        if io.function == "digital" and io.static in "01":
            line = self._dut_sees("SWDIO", io.vih if io.static == "1" else io.vil)
        self.dut_drive["SWDIO"] = self.chip.swd.cycle(line)

    def read_static(self, pins):
        out = []
        for p in pins:
            c = self._ch(p)
            v = self._line_voltage(p)
            out.append("H" if v > c.voh else "L" if v < c.vol else "M")
        return out

    # ---- PPMU ----
    def _other_v(self, pin):
        dut = self._dut_pin(pin)
        if dut and dut.fault and dut.fault.startswith("short:"):
            other = dut.fault.split(":", 1)[1]
            if other in self.channels:
                oc = self.channels[other]
                if oc.function == "ppmu" and oc.ppmu["sourcing"] and oc.ppmu["mode"] == "voltage":
                    return oc.ppmu["level"]
                return oc.vil if oc.static == "0" else oc.vih if oc.static == "1" else 0.0
        return None

    def _pin_current(self, pin, v):
        dut = self._dut_pin(pin)
        if dut is None:
            return 0.2e-9 * v
        return dut.current(v, self.chip.vdd, self._other_v(pin))

    def _ppmu_voltage(self, pin):
        c = self.channels[pin]
        m = c.ppmu
        if m["mode"] == "voltage":
            return m["level"]
        lo, hi = m["v_low"], m["v_high"]
        target = m["level"]
        if self._pin_current(pin, lo) > target:      # cannot pull the pin low enough: clamps at the limit
            return lo
        if self._pin_current(pin, hi) < target:
            return hi
        for _ in range(80):                          # I(v) is monotonic: bisection
            mid = 0.5 * (lo + hi)
            if self._pin_current(pin, mid) < target:
                lo = mid
            else:
                hi = mid
        return 0.5 * (lo + hi)

    def ppmu_configure(self, pins, mode, level, i_range, v_low=V_MIN, v_high=V_MAX, source=True):
        if i_range not in PPMU_RANGES:
            i_range = min(PPMU_RANGES, key=lambda r: abs(r - i_range))
        if mode == "current" and abs(level) > i_range:
            raise ValueError(f"current {level} A exceeds the {i_range} A range")
        if mode == "voltage" and not V_MIN <= level <= V_MAX:
            raise ValueError("voltage outside -2 … 6 V")
        for p in pins:
            c = self._ch(p)
            c.function = "ppmu"
            c.ppmu.update({"mode": mode, "level": float(level), "range": i_range,
                           "v_low": float(v_low), "v_high": float(v_high), "sourcing": bool(source)})

    def ppmu_measure(self, pins, what):
        out = []
        for p in pins:
            c = self._ch(p)
            if c.function != "ppmu" or not c.ppmu["sourcing"]:
                out.append(0.0)
                continue
            v = self._ppmu_voltage(p)
            if what == "voltage":
                out.append(v + self.rng.gauss(0, 150e-6))
            else:
                i = self._pin_current(p, v)
                rng = c.ppmu["range"]
                i = max(-rng, min(rng, i))
                out.append(i + self.rng.gauss(0, rng * 2e-4))
        return out

    def ppmu_off(self, pins):
        for p in pins:
            c = self._ch(p)
            c.ppmu["sourcing"] = False
            c.function = "off"

    # ---- patterns ----
    def load_pinmap(self, path):
        return {"pinmap": os.path.basename(path)}

    def load_project(self, folder):
        loaded = []
        for name in sorted(os.listdir(folder)):
            if name.endswith((".digipat", ".digipatsrc")):
                self.patterns[os.path.splitext(name)[0]] = os.path.join(folder, name)
                loaded.append(name)
        return {"loaded": loaded, "patterns": sorted(self.patterns)}

    def load_pattern(self, path):
        name = os.path.splitext(os.path.basename(path))[0]
        self.patterns[name] = path
        return name

    def burst(self, start_label, timeout=10.0):
        if start_label not in self.patterns:
            raise ValueError(f"pattern {start_label!r} is not loaded")
        pins = list(self.channels)
        vih = min(self.channels[p].vih for p in pins if p in ("SWCLK", "SWDIO")) if "SWCLK" in pins else 3.3
        fmax = self.chip.fmax(vih)
        f = 1.0 / self.period
        fails = {}
        if fmax <= 0 or f > fmax:
            sev = 1.0 if fmax <= 0 else min(1.0, (f / fmax - 1) * 3)
            for p in pins:
                if self._dut_pin(p) is not None:
                    fails[p] = int(sev * self.rng.randint(5, 200))
        for p in pins:                                # opens never toggle
            dut = self._dut_pin(p)
            if dut is not None and dut.fault in ("open", "short_gnd", "short_vdd"):
                fails[p] = fails.get(p, 0) + 64
        cycles = 4096
        return {"pass": not any(fails.values()), "fail_count": {p: fails.get(p, 0) for p in pins},
                "cycles": cycles, "time_s": cycles * self.period}

    # ---- project patterns (used by test tasks through the host) ----
    # The simulated instrument checks the files and keeps time; the task models the DUT.
    def project_open(self, pinmap, specs, levels, timing):
        for f in [pinmap] + list(specs or []) + list(levels or []) + list(timing or []):
            if not os.path.exists(f):
                raise FileNotFoundError(f)
        self.proj = {"timing_applied": (timing or [None])[0], "specs": list(specs or [])}
        self.run = None
        return {"pinmap": pinmap, "timing": list(timing or [])}

    def _proj(self):
        if not getattr(self, "proj", None):
            raise RuntimeError("no pattern project loaded (pattern_project first)")
        return self.proj

    def use_timing(self, path):
        if not os.path.exists(path):
            raise FileNotFoundError(path)
        self._proj()["timing_applied"] = path
        return {"applied": path}

    def set_specs(self, path):
        if not os.path.exists(path):
            raise FileNotFoundError(path)
        self._proj()["specs"] = [path]
        return {"specs": path}

    def burst_start(self, path, label, seconds):
        self._proj()
        if not os.path.exists(path):
            raise FileNotFoundError(path)
        self.run = {"t0": time.monotonic(), "seconds": max(0.0, float(seconds))}

    def burst_done(self):
        return self.run is None or time.monotonic() - self.run["t0"] >= self.run["seconds"]

    def burst_result(self, pins):
        return True, {}

    def burst_abort(self):
        self.run = None

    # ---- SWD over patterns (full-speed PHY) ----
    def swd_cycles(self, cycles, frequency):
        clk, io = self.channels.get("SWCLK"), self.channels.get("SWDIO")
        if clk is None or io is None:
            raise ValueError("pins SWCLK and SWDIO must be mapped")
        fmax = self.chip.fmax(min(clk.vih, io.vih))
        p_err = 0.0 if fmax > 0 and frequency <= fmax else (0.5 if fmax <= 0 else min(0.5, (frequency / fmax - 1) * 2))
        alive = self.chip.vdd >= 1.6 and clk.vih >= 0.7 * self.chip.vdd and self.chip.enabled
        dut_io = self._dut_pin("SWDIO")
        if dut_io is not None and dut_io.fault in ("open", "short_gnd", "short_vdd"):
            alive = False
        captured = []
        for c in cycles:
            line = None
            if c[0] == "w":
                bit = int(c[1])
                line = self._dut_sees("SWDIO", io.vih if bit else io.vil)
                if self.rng.random() < p_err:
                    line ^= 1
            out = self.chip.swd.cycle(line) if alive else None
            if c == "r":
                v = (self.chip.vdd if out else 0.0) if out is not None else (self.chip.vdd if alive else io.vterm)
                bit = 1 if v > io.voh else 0 if v < io.vol else self.rng.randint(0, 1)
                if self.rng.random() < p_err:
                    bit ^= 1
                captured.append(bit)
        return captured
