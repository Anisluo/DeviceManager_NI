"""Simulated 6½-digit DMM (Python plugin).

Measures a node of another device in the station (the "probe"), so a script can
drive a simulated SMU and read the result through an independent instrument,
the same way it will on the real bench (PXIe-4145 + PXIe-4081). Adds the DMM's
own error model: gain/offset error, range-dependent noise that falls with NPLC,
and a reading time of NPLC power-line cycles.
"""

import math
import random
import time

import dm
from dm.plugin import Plugin, PluginError, method, E_HW

RANGES = {"dcv": [0.1, 1.0, 10.0, 100.0, 1000.0],
          "dci": [1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1.0]}
LINE_HZ = 50.0


class SimDmm(Plugin):
    def __init__(self, config):
        super().__init__(config)
        opts = self.options
        self.function = "dcv"
        self.range = 10.0
        self.nplc = 1.0
        self.gain_ppm = float(opts.get("gain_ppm", 12.0))        # gain error
        self.offset = float(opts.get("offset_uv", 3.0)) * 1e-6   # offset error [V]
        self.noise_ppm_range = float(opts.get("noise_ppm_range", 0.3))   # rms noise at 1 NPLC, ppm of range
        self.probe = opts.get("probe")    # {"device","method","args","field"}
        self.manual_value = 0.0
        self.rng = random.Random(opts.get("seed"))
        self.count = 0
        self.log(f"{self.resource}: probe = {self._probe_text()}")

    def _probe_text(self):
        p = self.probe
        return f"{p['device']}.{p['method']}({p.get('args', {})})[{p['field']}]" if p else "manual"

    def _true_value(self):
        if not self.probe:
            return self.manual_value
        p = self.probe
        try:
            result = dm.Device(p["device"]).invoke(p["method"], p.get("args", {}))
        except Exception as e:  # noqa: BLE001
            raise PluginError(f"probe {self._probe_text()} failed: {e}", E_HW)
        return float(dm.plugin._clean(result).get(p["field"], float("nan")))

    # ------------------------------------------------------------------
    @method("Configure function (dcv | dci), range and NPLC")
    def configure(self, function: str = None, range: float = None, nplc: float = None):
        if function is not None:
            if function not in RANGES:
                raise ValueError("function must be dcv or dci")
            self.function = function
            if self.range not in RANGES[function]:
                self.range = RANGES[function][-1]
        if range is not None:
            fits = [r for r in RANGES[self.function] if r >= abs(range) * 0.999]
            if not fits:
                raise ValueError(f"range {range} too large for {self.function}")
            self.range = fits[0]                  # pick the smallest range that fits
        if nplc is not None:
            if not 0.01 <= nplc <= 100:
                raise ValueError("nplc must be in [0.01, 100]")
            self.nplc = float(nplc)
        return self.get_config()

    @method("Current configuration")
    def get_config(self):
        return {"function": self.function, "range": self.range, "nplc": self.nplc,
                "ranges": RANGES[self.function], "probe": self._probe_text()}

    @method("Value measured when no probe is configured")
    def set_manual_value(self, value: float):
        self.manual_value = float(value)

    @method("Take one reading")
    def read(self):
        time.sleep(self.nplc / LINE_HZ)           # integration time
        true = self._true_value()
        sigma = self.noise_ppm_range * 1e-6 * self.range / math.sqrt(self.nplc)
        value = true * (1 + self.gain_ppm * 1e-6) + self.offset + self.rng.gauss(0.0, sigma)
        self.count += 1
        overload = abs(value) > self.range * 1.2
        if overload:
            self.emit({"type": "state", "overload": True, "range": self.range})
        return {"value": float("nan") if overload else value, "overload": overload,
                "function": self.function, "range": self.range, "nplc": self.nplc, "n": self.count}

    @method("Average N readings; returns mean, standard deviation, min, max")
    def read_stats(self, count: int = 10):
        if not 1 <= count <= 10000:
            raise ValueError("count must be in [1, 10000]")
        values = [self.read()["value"] for _ in range(int(count))]
        mean = sum(values) / len(values)
        std = math.sqrt(sum((v - mean) ** 2 for v in values) / max(1, len(values) - 1))
        return {"mean": mean, "std": std, "min": min(values), "max": max(values), "count": len(values)}
