"""Multi-dimensional scan engine.

Nested scan — axes[0] is the outermost loop, axes[-1] the innermost::

    from dm import Axis, Scan, sweep
    smu, dmm = dm.device("smu1"), dm.device("dmm1")

    result = Scan(
        "line/load regulation",
        axes=[Axis("rload", [3300, 330, 110], set=lambda r: smu.set_load(channel=0, r_load=r), unit="Ω"),
              Axis("vin", sweep(3.0, 5.5, 0.1), set=lambda v: smu.set_voltage(channel=0, voltage=v),
                   settle=0.01, unit="V")],
        measure=lambda p: {"vout": dmm.read()["value"], "iin": smu.measure(channel=0)["current"]},
        setup=lambda: smu.set_output(channel=0, enabled=True),
        teardown=lambda: smu.set_output(channel=0, enabled=False),
    ).run()

Every point becomes one row (axis values + measured values) in a new data run
(kind "scan"), progress is reported automatically and Stop is honoured
between points. `teardown` always runs.

Shmoo — two axes and a pass/fail test::

    shmoo("VDD × SWCLK", x=Axis("fclk", ...), y=Axis("vdd", ...),
          test=lambda p: swd_read_idcode() == 0x0BB11477)

The test may return a bool, or a dict with a "pass" key plus extra values.
"""

import time

import dm

__all__ = ["Axis", "Scan", "ScanResult", "shmoo", "run_spec"]


class Axis:
    def __init__(self, name, values, set=None, settle=0.0, unit=""):
        self.name = name
        self.values = list(values)
        self.set = set
        self.settle = float(settle)
        self.unit = unit
        if not self.values:
            raise ValueError(f"axis {name!r} has no values")

    def describe(self):
        return {"name": self.name, "unit": self.unit, "count": len(self.values),
                "min": min(self.values), "max": max(self.values)}


class ScanResult:
    def __init__(self, run_id, rows, completed):
        self.run_id = run_id
        self.rows = rows
        self.completed = completed

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, column):
        return [r.get(column) for r in self.rows]

    def __repr__(self):
        return f"<ScanResult run {self.run_id}: {len(self.rows)} points{'' if self.completed else ' (incomplete)'}>"


def _as_dict(value, default_key):
    if isinstance(value, dict):
        return value
    if value is None:
        return {}
    return {default_key: value}


class Scan:
    def __init__(self, name, axes, measure, setup=None, teardown=None, snake=False,
                 kind="scan", meta=None, record=True):
        if not axes:
            raise ValueError("a scan needs at least one axis")
        self.name = name
        self.axes = list(axes)
        self.measure = measure
        self.setup = setup
        self.teardown = teardown
        self.snake = snake               # alternate the direction of the inner axes (fewer big jumps)
        self.kind = kind
        self.meta = dict(meta or {})
        self.record = record

    @property
    def total(self):
        n = 1
        for a in self.axes:
            n *= len(a.values)
        return n

    def _points(self):
        """Yield (point dict, changed axis indices) in scan order."""
        idx = [0] * len(self.axes)
        prev = None
        for flat in range(self.total):
            rem = flat
            for k in range(len(self.axes) - 1, -1, -1):
                n = len(self.axes[k].values)
                idx[k] = rem % n
                rem //= n
            point, values = {}, []
            for k, axis in enumerate(self.axes):
                i = idx[k]
                if self.snake and k > 0:
                    # reverse this axis on every other pass of the axis above it
                    outer = flat // (len(axis.values) * _inner_size(self.axes, k))
                    if outer % 2 == 1:
                        i = len(axis.values) - 1 - i
                point[axis.name] = axis.values[i]
                values.append(axis.values[i])
            changed = [k for k in range(len(values)) if prev is None or values[k] != prev[k]]
            prev = values
            yield point, changed

    def run(self):
        meta = dict(self.meta)
        meta["axes"] = [a.describe() for a in self.axes]
        meta.setdefault("snake", self.snake)
        run_id = dm.new_run(self.name, kind=self.kind, **meta) if self.record else 0
        rows, completed = [], False
        t0 = time.time()
        try:
            if self.setup:
                self.setup()
            for n, (point, changed) in enumerate(self._points()):
                for k in changed:                       # outer axes first
                    axis = self.axes[k]
                    if axis.set:
                        axis.set(point[axis.name])
                    if axis.settle > 0:
                        dm.sleep(axis.settle)
                measured = _as_dict(self.measure(dict(point)), "value")
                row = dict(point)
                row.update(measured)
                rows.append(row)
                if self.record:
                    dm.record(row)
                done = (n + 1) / self.total
                eta = (time.time() - t0) / done * (1 - done)
                dm.progress(done, f"{n + 1}/{self.total} · " + ", ".join(f"{k}={_fmt(v)}" for k, v in point.items())
                            + (f" · 剩余 {eta:.0f} s" if n > 2 else ""))
            completed = True
        finally:
            if self.teardown:
                self.teardown()
        return ScanResult(run_id, rows, completed)


def _inner_size(axes, k):
    n = 1
    for a in axes[k + 1:]:
        n *= len(a.values)
    return n


def _fmt(v):
    return f"{v:.4g}" if isinstance(v, float) else str(v)


def shmoo(name, x, y, test, setup=None, teardown=None, snake=True, meta=None):
    """2-D pass/fail map. y is the outer (slow) axis, x the inner one."""
    def measure(point):
        r = test(point)
        if isinstance(r, dict):
            r = dict(r)
            r["pass"] = bool(r.get("pass"))
            return r
        return {"pass": bool(r)}
    m = dict(meta or {})
    m.update({"x": x.name, "y": y.name, "value": "pass"})
    return Scan(name, axes=[y, x], measure=measure, setup=setup, teardown=teardown,
                snake=snake, kind="shmoo", meta=m).run()


# ---------------------------------------------------------------------------
# Declarative scans (page.json "sweep" widget)
# ---------------------------------------------------------------------------

def _substitute(obj, value):
    if obj == "$value":
        return value
    if isinstance(obj, list):
        return [_substitute(v, value) for v in obj]
    if isinstance(obj, dict):
        return {k: _substitute(v, value) for k, v in obj.items()}
    return obj


def _field(obj, path):
    for part in str(path).split(".") if path else []:
        obj = obj[int(part)] if isinstance(obj, list) else obj.get(part)
    return obj


def _values(a):
    if "values" in a:
        return list(a["values"])
    if "points" in a:
        return dm.linspace(float(a["start"]), float(a["stop"]), int(a["points"]))
    return dm.sweep(float(a["start"]), float(a["stop"]), float(a["step"]))


def _action(spec, self_device):
    dev = dm.Device(self_device if spec.get("device", "$self") == "$self" else spec["device"])
    return lambda value=None: dev.invoke(spec["method"], _substitute(spec.get("args", {}), value))


def run_spec(spec, self_device=None):
    """Run a scan described as JSON (dict or string). "$self" means the page's device.

    {"name": "...", "kind": "scan" | "shmoo",
     "axes": [{"name", "unit", "start", "stop", "step" | "points" | "values",
               "device", "method", "args": {..."$value"...}, "settle_ms"}],          # outer → inner
     "measure": [{"name", "device", "method", "args", "field"}],
     "pass": "vout > 3.2",                        # shmoo / limit check, Python expression over the row
     "setup": [{"device", "method", "args"}], "teardown": [...], "snake": false}
    """
    import json
    if isinstance(spec, str):
        spec = json.loads(spec)
    axes = []
    for a in spec["axes"]:
        setter = _action(a, self_device) if a.get("method") else None
        axes.append(Axis(a["name"], _values(a), set=setter, settle=a.get("settle_ms", 0) / 1000.0,
                         unit=a.get("unit", "")))
    # Probes with the same device/method/args share one call per point (e.g. vout and iin from one measure()).
    probes = [(m["name"], _action(m, self_device), m.get("field"),
               json.dumps([m.get("device", "$self"), m["method"], m.get("args", {})], sort_keys=True))
              for m in spec.get("measure", [])]
    criterion = spec.get("pass")
    if criterion:
        compiled = compile(criterion, "<pass>", "eval")

    def measure(point):
        row, cache = {}, {}
        for name, call, field, key in probes:
            if key not in cache:
                cache[key] = call()
            row[name] = _field(cache[key], field)
        if criterion:
            scope = dict(point)
            scope.update(row)
            try:
                row["pass"] = bool(eval(compiled, {"__builtins__": {}, "abs": abs, "min": min, "max": max}, scope))
            except Exception:  # noqa: BLE001 — e.g. a probe returned None
                row["pass"] = False
        return row

    def actions(key):
        steps = [_action(s, self_device) for s in spec.get(key, [])]
        return (lambda: [s() for s in steps]) if steps else None

    kind = spec.get("kind", "scan")
    meta = {"spec": spec, "device": self_device}
    if kind == "shmoo":
        if len(axes) != 2:
            raise ValueError("a shmoo needs exactly two axes (y outer, x inner)")
        meta.update({"x": axes[1].name, "y": axes[0].name, "value": "pass"})
    return Scan(spec.get("name", "sweep"), axes, measure, setup=actions("setup"), teardown=actions("teardown"),
                snake=spec.get("snake", kind == "shmoo"), kind=kind, meta=meta).run()
