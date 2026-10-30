"""Unit tests for dm.plugin — run without the host: python -m unittest discover sdk/python/tests"""

import json
import math
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from dm import sweep, linspace  # noqa: E402
from dm.plugin import Plugin, PluginError, method, E_BAD_ARGS, E_NO_METHOD, E_HW, E_FAIL  # noqa: E402


class Demo(Plugin):
    def __init__(self, config):
        super().__init__(config)
        self.level = 0.0

    @method("Set level")
    def set_level(self, level: float):
        if level < 0:
            raise ValueError("level must be >= 0")
        self.level = level

    @method
    def read(self, channel: int = 0):
        """Read one value."""
        return {"value": self.level, "channel": channel, "nan": float("nan")}

    @method("Always fails with a hardware error")
    def broken(self):
        raise PluginError("timeout", E_HW)

    @method("Crashes")
    def crash(self):
        return 1 / 0

    def helper(self):        # not exported
        return 42


def call(p, name, args=None):
    rc, text = p._dm_invoke_json(name, json.dumps(args or {}))
    return rc, json.loads(text)


class PluginTests(unittest.TestCase):
    def setUp(self):
        self.p = Demo({"resource": "X1", "options": {"a": 1}})
        self.events = []
        self.p._dm_set_emit(self.events.append)

    def test_config(self):
        self.assertEqual(self.p.resource, "X1")
        self.assertEqual(self.p.options, {"a": 1})

    def test_call_and_none_result(self):
        self.assertEqual(call(self.p, "set_level", {"level": 1.5}), (0, {"ok": True}))
        rc, r = call(self.p, "read", {"channel": 2})
        self.assertEqual(rc, 0)
        self.assertEqual(r["value"], 1.5)
        self.assertIsNone(r["nan"])            # NaN must not leak into JSON

    def test_methods_listing(self):
        rc, r = call(self.p, "$methods")
        names = {m["name"]: m for m in r}
        self.assertEqual(set(names), {"set_level", "read", "broken", "crash"})
        self.assertEqual(names["read"]["doc"], "Read one value.")
        self.assertEqual(names["set_level"]["args"], ["level"])

    def test_errors(self):
        self.assertEqual(call(self.p, "nope")[0], E_NO_METHOD)
        self.assertEqual(call(self.p, "helper")[0], E_NO_METHOD)
        self.assertEqual(call(self.p, "set_level", {"lvl": 1})[0], E_BAD_ARGS)     # unknown arg
        self.assertEqual(call(self.p, "set_level")[0], E_BAD_ARGS)                 # missing arg
        self.assertEqual(call(self.p, "set_level", {"level": -1})[0], E_BAD_ARGS)  # ValueError
        self.assertEqual(call(self.p, "broken")[0], E_HW)
        rc, r = call(self.p, "crash")
        self.assertEqual(rc, E_FAIL)
        self.assertIn("ZeroDivisionError", r["error"])
        rc, _ = self.p._dm_invoke_json("read", "{not json")
        self.assertEqual(rc, E_BAD_ARGS)

    def test_events(self):
        self.p.log("hello", "warning")
        self.assertEqual(json.loads(self.events[0]), {"type": "log", "level": "warning", "message": "hello"})


class HelperTests(unittest.TestCase):
    def test_sweep(self):
        self.assertEqual(len(sweep(0, 1, 0.1)), 11)
        self.assertEqual(sweep(0, 1, 0.1)[3], 0.3)
        self.assertEqual(sweep(5, 3, -1), [5, 4, 3])
        with self.assertRaises(ValueError):
            sweep(0, 1, -0.1)

    def test_linspace(self):
        xs = linspace(0, 1, 5)
        self.assertEqual(len(xs), 5)
        self.assertTrue(math.isclose(xs[-1], 1.0))


if __name__ == "__main__":
    unittest.main()
