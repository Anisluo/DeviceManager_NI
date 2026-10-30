"""PXIe-6571 through NI-Digital Pattern Driver (nimi-python `nidigital`).

Runs on the PXIe-8881 with NI-Digital installed. `simulate: true` in the
station config opens a driver-simulated 6571 (no hardware, but the real
driver), which is the best check of this file short of the instrument.

NOTE: written against the nimi-python nidigital API without access to a 6571;
items marked VERIFY need a check on the bench (see README).
"""

import glob
import hashlib
import json
import os
import subprocess
import xml.sax.saxutils as sx

CAPTURE_STATE = "X"   # VERIFY: pin state used on capture vectors (X = no compare, value still captured);
                      # override with options.capture_state ("V" is the other candidate)

# Command-line conventions tried in order until one produces the .digipat (VERIFY on the 8881);
# the one that works is remembered in <work_dir>/compiler.json. options.compile_args overrides.
COMPILE_ARG_CANDIDATES = [
    ["{src}", "-o", "{out}", "-pinmap", "{pinmap}"],
    ["-pinmap", "{pinmap}", "-o", "{out}", "{src}"],
    ["--pinmap", "{pinmap}", "--output", "{out}", "{src}"],
    ["/pinmap:{pinmap}", "/out:{out}", "{src}"],
    ["{src}", "{pinmap}", "{out}"],
]


def find_compiler(hint=None):
    """DigitalPatternCompiler.exe: options.pattern_compiler, else a search under National Instruments."""
    if hint and os.path.exists(hint):
        return hint
    roots = [os.environ.get("ProgramFiles", "C:/Program Files"), os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")]
    for root in roots:
        for depth in range(1, 4):
            pattern = os.path.join(root, "National Instruments", *(["*"] * depth), "DigitalPatternCompiler.exe")
            hits = glob.glob(pattern)
            if hits:
                return hits[0]
    return None


def _import():
    try:
        import nidigital
        import hightime
        return nidigital, hightime
    except ImportError as e:
        raise RuntimeError("nidigital (nimi-python) 未安装：在宿主所用的 Python 中 pip install nidigital；"
                           "并需要 NI-Digital Pattern Driver") from e


def write_pinmap(path, resource, pins, channels=32):
    """Pin map for one 6571 and one site. pins: {name: channel}."""
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<PinMap schemaVersion="1.4" xmlns="http://www.ni.com/TestStand/SemiconductorModule/PinMap.xsd">',
             '  <Instruments>',
             f'    <NIDigitalPatternInstrument name="{sx.escape(resource)}" numberOfChannels="{channels}" />',
             '  </Instruments>',
             '  <Pins>']
    lines += [f'    <DUTPin name="{sx.escape(p)}" />' for p in pins]
    lines += ['  </Pins>', '  <PinGroups />', '  <Sites>', '    <Site siteNumber="0" />', '  </Sites>',
              '  <Connections>']
    lines += [f'    <Connection pin="{sx.escape(p)}" siteNumber="0" instrument="{sx.escape(resource)}" channel="{ch}" />'
              for p, ch in pins.items()]
    lines += ['  </Connections>', '</PinMap>']
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    return path


def swd_pattern_source(name, shape, timeset="swd_ts", capture_state=CAPTURE_STATE):
    """digipatsrc for one SWD transfer shape ('w' sourced from swd_src, 'r' captured to swd_cap, 't' released)."""
    v = [f"file_format_version 1.1;", f"timeset {timeset};", "",
         f"pattern {name} (SWCLK, SWDIO)", "{"]
    if "w" in shape:
        v.append(f"    source_start(swd_src)  {timeset} 0 X;")
    if "r" in shape:
        v.append(f"    capture_start(swd_cap) {timeset} 0 X;")
    for c in shape:
        if c == "w":
            v.append(f"    source                 {timeset} 1 D;")
        elif c == "r":
            v.append(f"    capture                {timeset} 1 {capture_state};")
        else:
            v.append(f"                           {timeset} 1 X;")
    if "r" in shape:
        v.append(f"    capture_stop           {timeset} 0 X;")
    v.append(f"    halt                   {timeset} 0 X;")
    v.append("}")
    return "\n".join(v) + "\n"


class DriverBackend:
    name = "driver"

    def __init__(self, resource, pins, options):
        self.nidigital, self.hightime = _import()
        nd = self.nidigital
        self.resource = resource
        self.pins = dict(pins)
        self.options = options
        self.work = options.get("work_dir") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "_generated")
        os.makedirs(self.work, exist_ok=True)
        session_opts = {}
        if options.get("simulate"):
            session_opts = {"simulate": True, "driver_setup": {"Model": options.get("model", "6571")}}
        self.s = nd.Session(resource_name=resource, reset=bool(options.get("reset", False)), options=session_opts)
        self.pinmap = write_pinmap(os.path.join(self.work, "station.pinmap"), resource, self.pins)
        self.s.load_pin_map(self.pinmap)
        self.period = 100e-9
        self.swd_freq = None
        self.compiled = {}
        self.capture_state = options.get("capture_state", CAPTURE_STATE)
        self.compile_log = []          # (command, returncode, output) of every compiler call, for diagnosis

    def _p(self, pins):
        return self.s.pins[",".join(pins)]

    def info(self):
        s = self.s
        return {"backend": "driver", "simulated": bool(self.options.get("simulate")), "resource": self.resource,
                "model": getattr(s, "instrument_model", "PXIe-6571"),
                "serial": getattr(s, "serial_number", ""), "driver": getattr(s, "specific_driver_revision", ""),
                "channels": 32}

    def close(self):
        try:
            self.s.close()
        except Exception:  # noqa: BLE001
            pass

    # ---- levels & timing ----
    def set_levels(self, pins, vil=None, vih=None, vol=None, voh=None, vterm=None):
        ch = self._p(pins)
        for k, v in (("vil", vil), ("vih", vih), ("vol", vol), ("voh", voh), ("vterm", vterm)):
            if v is not None:
                setattr(ch, k, float(v))

    def get_levels(self, pin):
        ch = self.s.pins[pin]
        return {k: float(getattr(ch, k)) for k in ("vil", "vih", "vol", "voh", "vterm")}

    def set_period(self, period):
        self.period = float(period)   # applied to time sets created by this plugin (e.g. SWD)

    # ---- static ----
    def select_digital(self, pins):
        self._p(pins).selected_function = self.nidigital.SelectedFunction.DIGITAL

    def write_static(self, pins, state):
        nd = self.nidigital
        st = {"0": nd.WriteStaticPinState.ZERO, "1": nd.WriteStaticPinState.ONE, "X": nd.WriteStaticPinState.X}[str(state).upper()]
        self._p(pins).write_static(st)

    def read_static(self, pins):
        states = self._p(pins).read_static()
        return [str(getattr(s, "name", s))[-1] for s in states]      # PinState.H -> "H"

    # ---- PPMU ----
    def ppmu_configure(self, pins, mode, level, i_range, v_low=-2.0, v_high=6.0, source=True):
        nd = self.nidigital
        ch = self._p(pins)
        ch.selected_function = nd.SelectedFunction.PPMU
        if mode == "voltage":
            ch.ppmu_output_function = nd.PPMUOutputFunction.VOLTAGE
            ch.ppmu_voltage_level = float(level)
            ch.ppmu_current_limit_range = float(i_range)
        else:
            ch.ppmu_output_function = nd.PPMUOutputFunction.CURRENT
            ch.ppmu_current_level_range = float(i_range)
            ch.ppmu_current_level = float(level)
            ch.ppmu_voltage_limit_low = float(v_low)
            ch.ppmu_voltage_limit_high = float(v_high)
        if "aperture" in self.options:
            ch.ppmu_aperture_time = float(self.options["aperture"])
        if source:
            ch.ppmu_source()

    def ppmu_measure(self, pins, what):
        nd = self.nidigital
        kind = nd.PPMUMeasurementType.VOLTAGE if what == "voltage" else nd.PPMUMeasurementType.CURRENT
        # VERIFY: result order follows the pin list order for a single site
        return [float(v) for v in self._p(pins).ppmu_measure(kind)]

    def ppmu_off(self, pins):
        self._p(pins).selected_function = self.nidigital.SelectedFunction.DISCONNECT

    # ---- patterns ----
    def load_pinmap(self, path):
        self.s.load_pin_map(path)
        self.pinmap = path
        return {"pinmap": path}

    def load_project(self, folder):
        """Loads *.pinmap, *.specs, *.digilevels, *.digitiming, *.digipat from a folder and applies the
        first levels/timing sheets."""
        files = sorted(os.listdir(folder))
        pick = lambda ext: [os.path.join(folder, f) for f in files if f.endswith(ext)]
        pinmaps = pick(".pinmap")
        if pinmaps:
            self.load_pinmap(pinmaps[0])
        specs, levels, timing = pick(".specs"), pick(".digilevels"), pick(".digitiming")
        if specs or levels or timing:
            self.s.load_specifications_levels_and_timing(specs or None, levels or None, timing or None)
        if levels and timing:
            self.s.apply_levels_and_timing(os.path.splitext(os.path.basename(levels[0]))[0],
                                           os.path.splitext(os.path.basename(timing[0]))[0])
        pats = pick(".digipat")
        for p in pats:
            self.s.load_pattern(p)
        return {"pinmap": pinmaps[:1], "specs": specs, "levels": levels, "timing": timing,
                "loaded": [os.path.basename(p) for p in pats]}

    def load_pattern(self, path):
        self.s.load_pattern(path)
        return os.path.splitext(os.path.basename(path))[0]

    def burst(self, start_label, timeout=10.0):
        result = self.s.burst_pattern(start_label, select_digital_function=True, wait_until_done=True,
                                      timeout=self.hightime.timedelta(seconds=timeout))
        passed = all(result.values()) if isinstance(result, dict) else bool(result)
        fails = {}
        for p in self.pins:
            try:
                fc = self.s.pins[p].get_fail_count()
                fails[p] = int(fc[0] if isinstance(fc, (list, tuple)) else fc)
            except Exception:  # noqa: BLE001 — pins not in the pattern
                fails[p] = 0
        return {"pass": passed, "fail_count": fails}

    # ---- project patterns (used by test tasks through the host) ----
    def project_open(self, pinmap, specs, levels, timing):
        """Load a Digital Pattern Editor project's sheets; replaces the station pin map."""
        self.s.load_pin_map(pinmap)
        self.pinmap = pinmap
        self.compiled = {}                              # SWD patterns belong to the old pin map
        self.s.load_specifications_levels_and_timing(specs or None, levels or None, timing or None)
        self.proj = {"levels": (levels or [None])[0], "specs": list(specs or []),
                     "timing_loaded": set(timing or []), "timing_applied": None, "loaded": None}
        if levels and timing:
            self.use_timing(timing[0])
        return {"pinmap": pinmap, "timing": list(timing or [])}

    def _sheet(self, path):
        return os.path.splitext(os.path.basename(path))[0]

    def use_timing(self, path):
        pr = self._proj()
        if path not in pr["timing_loaded"]:
            self.s.load_specifications_levels_and_timing(None, None, [path])
            pr["timing_loaded"].add(path)
        if path != pr["timing_applied"] and pr["levels"]:
            self.s.apply_levels_and_timing(self._sheet(pr["levels"]), self._sheet(path))
            pr["timing_applied"] = path
        return {"applied": path}

    def set_specs(self, path):
        pr = self._proj()
        try:
            if pr["specs"]:
                self.s.unload_specifications(pr["specs"])
        except Exception:  # noqa: BLE001 — older drivers: load over the old sheet
            pass
        self.s.load_specifications_levels_and_timing([path], None, None)
        pr["specs"] = [path]
        if pr["timing_applied"] and pr["levels"]:
            self.s.apply_levels_and_timing(self._sheet(pr["levels"]), self._sheet(pr["timing_applied"]))
        return {"specs": path}

    def _proj(self):
        if not getattr(self, "proj", None):
            raise RuntimeError("no pattern project loaded (pattern_project first)")
        return self.proj

    def burst_start(self, path, label, seconds):
        pr = self._proj()
        if pr["loaded"] != path:
            self.s.unload_all_patterns()
            self.s.load_pattern(path)
            pr["loaded"] = path
        self.s.burst_pattern(label, select_digital_function=True, wait_until_done=False)

    def burst_done(self):
        return bool(self.s.is_done())

    def burst_result(self, pins):
        try:
            sites = self.s.get_site_pass_fail()
            passed = all(sites.values()) if isinstance(sites, dict) else bool(sites)
        except Exception:  # noqa: BLE001
            passed = None
        fails = {}
        for p in pins:
            try:
                fc = self.s.pins[p].get_fail_count()
                fails[p] = int(fc[0] if isinstance(fc, (list, tuple)) else fc)
            except Exception:  # noqa: BLE001 — pin not in the pattern
                pass
        return passed, fails

    def burst_abort(self):
        self.s.abort()

    # ---- SWD over patterns ----
    def _swd_timeset(self, frequency):
        nd, ht = self.nidigital, self.hightime
        if self.swd_freq == frequency:
            return
        T = 1.0 / frequency
        ts = "swd_ts"
        try:
            self.s.create_time_set(ts)
        except Exception:  # noqa: BLE001 — already exists
            pass
        self.s.configure_time_set_period(ts, ht.timedelta(seconds=T))
        sec = lambda x: ht.timedelta(seconds=x)
        # SWCLK: return-to-low pulse, rising at T/2. SWDIO: NRZ from t=0, strobe late in the cycle.
        self.s.pins["SWCLK"].configure_time_set_drive_edges(ts, nd.DriveFormat.RL, sec(0), sec(T / 2), sec(T), sec(T))
        self.s.pins["SWDIO"].configure_time_set_drive_edges(ts, nd.DriveFormat.NR, sec(0), sec(0), sec(T), sec(T))
        self.s.pins["SWDIO"].configure_time_set_compare_edges_strobe(ts, sec(0.9 * T))
        self.swd_freq = frequency

    def compile_pattern(self, src, out):
        """Compile a .digipatsrc against the current pin map, trying the known command lines."""
        compiler = find_compiler(self.options.get("pattern_compiler"))
        if not compiler:
            raise RuntimeError("DigitalPatternCompiler.exe not found — set options.pattern_compiler")
        memo = os.path.join(self.work, "compiler.json")
        if "compile_args" in self.options:
            candidates = [self.options["compile_args"]]
        else:
            candidates = list(COMPILE_ARG_CANDIDATES)
            if os.path.exists(memo):
                with open(memo, encoding="utf-8") as fh:
                    candidates.insert(0, json.load(fh)["args"])
        for args in candidates:
            cmd = [compiler] + [a.format(src=src, out=out, pinmap=self.pinmap) for a in args]
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
                self.compile_log.append((cmd, r.returncode, (r.stdout + r.stderr).strip()))
            except Exception as e:  # noqa: BLE001
                self.compile_log.append((cmd, -1, str(e)))
                continue
            if r.returncode == 0 and os.path.exists(out):
                with open(memo, "w", encoding="utf-8") as fh:
                    json.dump({"compiler": compiler, "args": args}, fh)
                return out
        cmd, rc, text = self.compile_log[-1]
        raise RuntimeError(f"pattern compile failed with every known command line; last: {' '.join(cmd)} -> {rc}: {text[:400]}")

    def _compile(self, shape):
        if shape in self.compiled:
            return self.compiled[shape]
        name = "swd_" + hashlib.sha1(f"{shape}|{self.capture_state}".encode()).hexdigest()[:10]
        src = os.path.join(self.work, name + ".digipatsrc")
        out = os.path.join(self.work, name + ".digipat")
        if not os.path.exists(out):
            with open(src, "w", encoding="utf-8") as fh:
                fh.write(swd_pattern_source(name, shape, capture_state=self.capture_state))
            self.compile_pattern(src, out)
        self.s.load_pattern(out)
        self.compiled[shape] = name
        return name

    def swd_cycles(self, cycles, frequency):
        nd = self.nidigital
        self._swd_timeset(frequency)
        shape = "".join("w" if c[0] == "w" else c for c in cycles)
        name = self._compile(shape)
        out_bits = [int(c[1]) for c in cycles if c[0] == "w"]
        n_cap = shape.count("r")
        if out_bits:
            try:
                self.s.pins["SWDIO"].create_source_waveform_serial("swd_src", nd.SourceDataMapping.BROADCAST, 1, nd.BitOrder.LSB)
            except Exception:  # noqa: BLE001 — already created
                pass
            self.s.write_source_waveform_broadcast("swd_src", out_bits)
        if n_cap:
            try:
                self.s.pins["SWDIO"].create_capture_waveform_serial("swd_cap", 1, nd.BitOrder.LSB)
            except Exception:  # noqa: BLE001
                pass
        self.s.burst_pattern(name, select_digital_function=True, wait_until_done=True,
                             timeout=self.hightime.timedelta(seconds=5))
        if not n_cap:
            return []
        data = self.s.fetch_capture_waveform("swd_cap", n_cap, timeout=self.hightime.timedelta(seconds=5))
        site = data[0] if isinstance(data, dict) else data
        return [int(b) & 1 for b in list(site)[:n_cap]]
