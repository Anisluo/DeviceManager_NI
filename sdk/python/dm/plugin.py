"""Base class for Python plugins.

A Python plugin has the same semantics as a C++ one (manifest / create /
invoke / destroy), only the transport differs::

    from dm.plugin import Plugin, PluginError, method

    class SimDmm(Plugin):
        def __init__(self, config):
            super().__init__(config)
            self.range = 10.0

        @method("Set the measurement range")
        def configure(self, range: float = 10.0):
            if range <= 0:
                raise ValueError("range must be > 0")       # -> DM_E_BAD_ARGS
            self.range = range                              # returning None -> {"ok": true}

        @method("Take one reading")
        def read(self):
            return {"value": 1.234}

plugin.json: ``"type": "python", "entry": "sim_dmm:SimDmm"``.

Methods receive the JSON args as keyword arguments. Unknown or missing
arguments are rejected with DM_E_BAD_ARGS before the method runs.
"""

import inspect
import json
import math

# Return codes, same values as dm_plugin.h
OK = 0
E_FAIL = -1
E_BUFFER = -2
E_NO_METHOD = -3
E_BAD_ARGS = -4
E_HW = -5
E_STATE = -6

__all__ = ["Plugin", "PluginError", "method", "to_json",
           "OK", "E_FAIL", "E_NO_METHOD", "E_BAD_ARGS", "E_HW", "E_STATE"]


class PluginError(Exception):
    """Raise from a method to return a specific ABI error code."""

    def __init__(self, message, code=E_FAIL):
        super().__init__(message)
        self.code = code


def method(doc=None, *, name=None):
    """Mark a Plugin method as callable by the host. Usable as @method or @method("doc")."""
    def mark(fn, text=None):
        fn._dm_method = name or fn.__name__
        fn._dm_doc = text if text is not None else (inspect.getdoc(fn) or "")
        return fn
    if callable(doc):                       # bare @method
        return mark(doc)
    return lambda fn: mark(fn, doc)


def _clean(value):
    """JSON-safe copy: NaN/inf -> None, numpy scalars/arrays -> Python types."""
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, dict):
        return {str(k): _clean(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_clean(v) for v in value]
    if hasattr(value, "tolist"):            # numpy array / scalar
        return _clean(value.tolist())
    if hasattr(value, "item"):
        return _clean(value.item())
    return value


def to_json(value):
    return json.dumps(_clean(value), ensure_ascii=False)


class Plugin:
    """Derive from this class; one instance per device session."""

    def __init__(self, config):
        self.config = config or {}
        self.resource = self.config.get("resource", "")
        self.simulate = bool(self.config.get("simulate", True))
        self.options = self.config.get("options", {}) or {}
        self._emit = None
        self._early_events = []      # emitted before the host connected (e.g. in __init__)

    # ---- events -------------------------------------------------------
    def emit(self, event):
        """Send an async event to the host, e.g. {"type": "state", ...}."""
        if self._emit is not None:
            self._emit(to_json(event))
        elif self._early_events is not None and len(self._early_events) < 100:
            self._early_events.append(to_json(event))

    def log(self, message, level="info"):
        self.emit({"type": "log", "level": level, "message": str(message)})

    def close(self):
        """Called before the instance is destroyed; release hardware here."""

    # ---- host side (do not override) -------------------------------------
    @classmethod
    def _dm_methods(cls):
        table = {}
        for klass in reversed(cls.__mro__):
            for attr in vars(klass).values():
                name = getattr(attr, "_dm_method", None)
                if name:
                    table[name] = attr
        return table

    def _dm_describe(self):
        out = []
        for name, fn in self._dm_methods().items():
            params = [p for p in inspect.signature(fn).parameters.values()
                      if p.name != "self" and p.kind in (p.POSITIONAL_OR_KEYWORD, p.KEYWORD_ONLY)]
            out.append({"name": name, "doc": fn._dm_doc, "args": [p.name for p in params]})
        return out

    def _dm_set_emit(self, fn):
        self._emit = fn
        if fn is not None and self._early_events:
            for evt in self._early_events:
                fn(evt)
        self._early_events = None

    def _dm_invoke_json(self, name, args_json):
        """Returns (rc, json_text). Never raises."""
        try:
            args = json.loads(args_json) if args_json else {}
            if not isinstance(args, dict):
                raise PluginError("args must be a JSON object", E_BAD_ARGS)
            if name == "$methods":
                return OK, to_json(self._dm_describe())
            fn = self._dm_methods().get(name)
            if fn is None:
                raise PluginError("unknown method: " + name, E_NO_METHOD)
            try:
                inspect.signature(fn).bind(self, **args)
            except TypeError as e:
                raise PluginError(f"{name}: {e}", E_BAD_ARGS)
            result = fn(self, **args)
            return OK, to_json({"ok": True} if result is None else result)
        except PluginError as e:
            return e.code, to_json({"error": str(e)})
        except ValueError as e:
            return E_BAD_ARGS, to_json({"error": str(e)})
        except Exception as e:  # noqa: BLE001 — the host must never see a raw exception
            return E_FAIL, to_json({"error": f"{type(e).__name__}: {e}"})
