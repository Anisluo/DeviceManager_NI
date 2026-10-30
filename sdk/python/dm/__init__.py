"""DeviceManager scripting API.

Inside the host (script console, sequence files, Python plugins)::

    import dm
    smu = dm.device("smu1")
    smu.invoke("set_voltage", {"channel": 0, "voltage": 1.2})   # explicit form
    smu.set_output(channel=0, enabled=True)                     # attribute form
    m = smu.measure(channel=0)
    dm.record(vin=1.2, iin=m["current"])                        # row in the data table
    dm.sleep(0.1)                                               # honours the Stop button

    from dm import Axis, Scan, shmoo                            # scan engine (dm.scan)
    dm.data.load(run_id)                                        # read stored runs (dm.data)
    dm.report.export(run_id, fmt="pdf")                         # datasheet-style report (dm.report)

`dm.plugin` (the plugin base class) also works outside the host, e.g. in unit tests.
"""

import json

from .plugin import Plugin, PluginError, method  # noqa: F401  (re-exported)

__all__ = ["device", "devices", "record", "sleep", "log", "progress", "linspace", "sweep",
           "new_run", "current_run", "Device", "DeviceError", "Plugin", "PluginError", "method",
           "Axis", "Scan", "shmoo"]


def _bindings():
    try:
        import _dmhost
    except ImportError as e:  # pragma: no cover
        raise RuntimeError("dm.* host functions are only available inside DeviceManager") from e
    return _dmhost


class DeviceError(RuntimeError):
    def __init__(self, device, method, code, message):
        super().__init__(f"{device}.{method}: {message} (code {code})")
        self.device, self.method, self.code = device, method, code


class Device:
    """Proxy for a device of the active station."""

    def __init__(self, name):
        self.name = name

    def invoke(self, method, args=None, **kwargs):
        payload = dict(args or {})
        payload.update(kwargs)
        rc, text = _bindings().invoke(self.name, method, json.dumps(payload))
        result = json.loads(text) if text else None
        if rc < 0:
            message = result.get("error", "failed") if isinstance(result, dict) else str(result)
            raise DeviceError(self.name, method, rc, message)
        return result

    def methods(self):
        return self.invoke("$methods")

    def __getattr__(self, method):
        if method.startswith("_"):
            raise AttributeError(method)
        return lambda args=None, **kw: self.invoke(method, args, **kw)

    def __repr__(self):
        return f"<dm.Device {self.name}>"


def devices():
    """Names of the devices in the active station."""
    return [d["name"] for d in _bindings().devices()]


def device(name):
    if name not in devices():
        raise KeyError(f"no device named {name!r}; available: {', '.join(devices())}")
    return Device(name)


def record(values=None, **kwargs):
    """Append one row to the current run's data table."""
    row = dict(values or {})
    row.update(kwargs)
    from .plugin import to_json
    _bindings().record(to_json(list(row.items())))     # pairs keep the column order


def new_run(name, kind="script", **meta):
    """Close the current data run and start a new one; returns its id.

    Without this, a script's records go to one run named after the script."""
    from .plugin import to_json
    return _bindings().new_run(str(name), str(kind), to_json(meta))


def current_run():
    """Id of the data run being written (0 before the first record)."""
    return _bindings().current_run()


def sleep(seconds):
    """Like time.sleep, but stops promptly when the user presses Stop."""
    _bindings().sleep(float(seconds))


def log(message, level="info"):
    _bindings().log(level, str(message))


def progress(fraction, text=""):
    """Report progress 0..1 to the script console."""
    _bindings().progress(float(fraction), str(text))


def linspace(start, stop, num):
    if num < 2:
        return [float(start)]
    step = (stop - start) / (num - 1)
    return [start + i * step for i in range(num)]


def sweep(start, stop, step):
    """Inclusive arithmetic sweep, robust against float accumulation: sweep(0, 1, 0.1) -> 11 points."""
    if step == 0:
        raise ValueError("step must not be 0")
    n = int(round((stop - start) / step))
    if n < 0:
        raise ValueError("step has the wrong sign for this range")
    return [round(start + i * step, 12) for i in range(n + 1)]


# Scan engine (imported last: it uses the functions above).
from .scan import Axis, Scan, shmoo  # noqa: E402,F401
