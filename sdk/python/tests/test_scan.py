"""Scan engine + report tests with a fake host (no DeviceManager needed)."""

import contextlib
import json
import os
import sqlite3
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))


class FakeHost(types.ModuleType):
    """Stands in for the embedded _dmhost module; stores runs in a real SQLite file."""

    def __init__(self, db):
        super().__init__("_dmhost")
        self.db = db
        self.run = 0
        self.calls = []
        self.exported = []
        c = sqlite3.connect(db)
        c.executescript("""
            CREATE TABLE runs (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, kind TEXT, station TEXT,
              script TEXT, started TEXT, finished TEXT, status TEXT, columns TEXT DEFAULT '[]',
              meta TEXT DEFAULT '{}', points INTEGER DEFAULT 0);
            CREATE TABLE points (run_id INTEGER, seq INTEGER, t REAL, data TEXT, PRIMARY KEY (run_id, seq));
        """)
        c.close()

    # --- bindings used by dm ---
    def new_run(self, name, kind, meta):
        with contextlib.closing(sqlite3.connect(self.db)) as c, c:
            cur = c.execute("INSERT INTO runs (name, kind, started, status, meta) VALUES (?, ?, '2026-10-29T10:00:00', 'running', ?)",
                            (name, kind, meta))
            self.run = cur.lastrowid
        return self.run

    def current_run(self):
        return self.run

    def record(self, pairs_json):
        pairs = json.loads(pairs_json)
        row = dict(pairs)
        with contextlib.closing(sqlite3.connect(self.db)) as c, c:
            n, cols = c.execute("SELECT points, columns FROM runs WHERE id = ?", (self.run,)).fetchone()
            cols = json.loads(cols)
            cols += [k for k, _ in pairs if k not in cols]
            c.execute("INSERT INTO points VALUES (?, ?, 0, ?)", (self.run, n, json.dumps(row)))
            c.execute("UPDATE runs SET points = ?, columns = ? WHERE id = ?", (n + 1, json.dumps(cols), self.run))

    def invoke(self, device, method, args):
        self.calls.append((device, method, json.loads(args)))
        a = json.loads(args)
        if method == "measure":
            v = self.state.get("v", 0.0)
            return 0, json.dumps({"load_v": min(3.3, max(0.0, v - 0.2)), "current": v / 1000})
        if method == "set_voltage":
            self.state["v"] = a["voltage"]
        return 0, json.dumps({"ok": True})

    state = {}

    def sleep(self, s):
        pass

    def progress(self, f, text):
        self.last_progress = f

    def db_path(self):
        return self.db

    def export_dir(self):
        return os.path.dirname(self.db)

    def file_exported(self, path):
        self.exported.append(path)


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.host = FakeHost(os.path.join(self.tmp.name, "t.sqlite"))
        sys.modules["_dmhost"] = self.host
        import dm  # noqa: F401
        import dm.scan
        self.scan = dm.scan

    def tearDown(self):
        sys.modules.pop("_dmhost", None)
        self.tmp.cleanup()

    def test_nested_order_and_setters(self):
        log = []
        s = self.scan.Scan("t", axes=[self.scan.Axis("a", [1, 2], set=lambda v: log.append(("a", v))),
                                      self.scan.Axis("b", [10, 20, 30], set=lambda v: log.append(("b", v)))],
                           measure=lambda p: p["a"] * p["b"])
        r = s.run()
        self.assertEqual([(row["a"], row["b"]) for row in r.rows],
                         [(1, 10), (1, 20), (1, 30), (2, 10), (2, 20), (2, 30)])
        self.assertEqual(r["value"], [10, 20, 30, 20, 40, 60])
        # outer axis set only when it changes; inner every point
        self.assertEqual(log.count(("a", 1)), 1)
        self.assertEqual(len([x for x in log if x[0] == "b"]), 6)
        self.assertTrue(r.completed)
        self.assertEqual(self.host.last_progress, 1.0)

    def test_snake(self):
        s = self.scan.Scan("t", axes=[self.scan.Axis("y", [0, 1, 2]), self.scan.Axis("x", [0, 1])],
                           measure=lambda p: None, snake=True)
        self.assertEqual([(p["y"], p["x"]) for p, _ in s._points()],
                         [(0, 0), (0, 1), (1, 1), (1, 0), (2, 0), (2, 1)])

    def test_teardown_runs_on_error(self):
        done = []
        def boom(p):
            raise RuntimeError("probe failed")
        s = self.scan.Scan("t", axes=[self.scan.Axis("a", [1])], measure=boom, teardown=lambda: done.append(1))
        with self.assertRaises(RuntimeError):
            s.run()
        self.assertEqual(done, [1])

    def test_shmoo_and_spec(self):
        spec = {"name": "spec", "kind": "shmoo",
                "axes": [{"name": "rl", "values": [100, 1000]},
                         {"name": "vin", "start": 3.0, "stop": 4.0, "step": 0.5, "device": "$self",
                          "method": "set_voltage", "args": {"channel": 0, "voltage": "$value"}}],
                "measure": [{"name": "vout", "device": "$self", "method": "measure", "args": {"channel": 0}, "field": "load_v"}],
                "pass": "vout > 3.25"}
        r = self.scan.run_spec(json.dumps(spec), self_device="smu1")
        self.assertEqual(len(r), 6)
        self.assertEqual(sorted(set(r["vin"])), [3.0, 3.5, 4.0])
        for row in r.rows:
            self.assertEqual(row["pass"], row["vout"] > 3.25)
        self.assertTrue(any(c[1] == "set_voltage" and c[0] == "smu1" for c in self.host.calls))

        import dm.data
        t = dm.data.load(r.run_id)
        self.assertEqual(t.columns[:2], ["rl", "vin"])
        self.assertEqual(t.run["kind"], "shmoo")
        self.assertEqual(t.run["meta"]["x"], "vin")

    @unittest.skipUnless(__import__("importlib").util.find_spec("matplotlib"), "matplotlib not installed")
    def test_reports(self):
        s = self.scan.Scan("线性调整率", axes=[self.scan.Axis("rl", [330, 3300], unit="Ω"),
                                          self.scan.Axis("vin", [3.0, 3.5, 4.0, 4.5], unit="V")],
                           measure=lambda p: {"vout": min(3.3, p["vin"] - 0.2), "iin": p["vin"] / p["rl"]})
        r = s.run()
        import dm.report
        for fmt in ("html", "pdf"):
            path = dm.report.export(r.run_id, fmt=fmt)
            self.assertTrue(os.path.getsize(path) > 2000, path)
        with open(self.host.exported[0], encoding="utf-8") as fh:
            html = fh.read()
        self.assertIn("<svg", html)
        self.assertIn("vout vs vin", html)


if __name__ == "__main__":
    unittest.main()
