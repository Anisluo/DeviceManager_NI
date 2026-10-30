"""tdc_6571 on the bundled DemoPrj-TDC project, simulated TDC.
Run: python -m unittest discover -s devices/tdc_6571/tests"""

import json
import os
import shutil
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
sys.path[:0] = [PLUGIN, os.path.join(PLUGIN, "..", "..", "sdk", "python")]

import digiproj as dp  # noqa: E402
import tdcgen  # noqa: E402
import tdms_lite  # noqa: E402
from tdc_6571 import Tdc6571  # noqa: E402

PROJECT = os.path.join(PLUGIN, "project", "DemoPrj-TDC")


def call(p, method_name, **args):
    rc, text = p._dm_invoke_json(method_name, json.dumps(args))
    r = json.loads(text)
    if rc < 0:
        raise AssertionError(f"{method_name} -> {rc}: {r}")
    return r


class Decode(unittest.TestCase):
    def test_tdms_and_vectors(self):
        p = dp.Pattern(os.path.join(PROJECT, "Patterns", "TDC_M20_2.digipat"))
        self.assertEqual(p.name, "TDC_M20_2")
        self.assertEqual(p.pins, ["I0", "I1", "I2", "I3"])
        self.assertEqual(len(p.base_ins), 21)
        self.assertEqual(p.instruction_text(1), "set_loop(60000)")
        self.assertEqual(p.instruction_text(18), "end_loop(wait1s)")
        self.assertEqual(p.instruction_text(20), "halt")
        self.assertEqual(p.states[4], "H001")          # waits for I0 (BUSY) high; I3 held at 1
        self.assertEqual(p.states[6], "X101")          # SendStart drives I1
        self.assertEqual(p.states[11], "X011")         # SendHit drives I2
        self.assertEqual(p.label_at(11), "SendHit")
        self.assertEqual(p.comments[0], "I1->start, I2->Stop")

    def test_single_pin_nibble(self):
        p = dp.Pattern(os.path.join(PROJECT, "Patterns", "TDC_1018.digipat"))
        self.assertEqual(p.states[3], "1")             # one pin: low nibble

    def test_project_sheets(self):
        pr = dp.Project(PROJECT)
        self.assertEqual(len(pr.patterns), 24)
        self.assertAlmostEqual(pr.symbols()["DC.VDD"], 3.3)
        self.assertAlmostEqual(pr.periods["ten_ns"], 10e-9)
        self.assertEqual(pr.pinmap["connections"]["stop"]["channel"], 15)
        self.assertAlmostEqual(dp.evaluate("0.4*DC.VDD", pr.symbols()), 1.32)
        self.assertAlmostEqual(dp.evaluate("-15 mA", {}), -0.015)


class Timing(unittest.TestCase):
    def setUp(self):
        self.pr = dp.Project(PROJECT)

    def analyze(self, name):
        return dp.analyze(self.pr.patterns[name], self.pr.periods, 120e-6)

    def test_m20_2(self):
        a = self.analyze("TDC_M20_2")
        self.assertEqual((a["start_pin"], a["hit_pin"], a["busy_pin"]), ("I1", "I2", "I0"))
        self.assertEqual(a["hit_count"], 16)
        self.assertAlmostEqual(a["hit_period"], 2e-6, delta=1e-9)
        self.assertAlmostEqual(a["start_to_hit"], 200.01e-6, delta=0.02e-6)
        self.assertEqual(a["loops"], 60000)
        self.assertAlmostEqual(a["total"], a["iteration"] * 60000, delta=a["iteration"] * 2)

    def test_hit_only_loop_is_not_an_outer_loop(self):
        a = self.analyze("TDC")
        self.assertEqual(a["hit_count"], 16)
        self.assertEqual(a["loops"], 1)

    def test_start_is_first_pin_to_rise(self):
        a = self.analyze("TDC_002")                    # START toggles 100x, then 16 HITs
        self.assertEqual((a["start_pin"], a["hit_pin"]), ("I1", "I2"))
        self.assertEqual(a["hit_count"], 16)

    def test_suspicious_loop_label_is_flagged(self):
        a = self.analyze("TDC_Mode3")
        self.assertTrue(any("v15" in n for n in a["notes"]))

    def test_all_patterns_analyze_quickly(self):
        t0 = time.perf_counter()
        for p in self.pr.patterns.values():
            dp.analyze(p, self.pr.periods)
        self.assertLess(time.perf_counter() - t0, 3.0)


class Edits(unittest.TestCase):
    def test_patch_count_changes_one_word(self):
        p = dp.Pattern(os.path.join(PROJECT, "Patterns", "TDC_M20_2.digipat"))
        applied, clamped = p.set_count(10, 8)          # set_loop(16) -> 8 HITs
        self.assertEqual((applied, clamped), (8, False))
        data = p.patched_bytes()
        self.assertEqual(len(data), len(p.data))
        self.assertEqual(sum(a != b for a, b in zip(data, p.data)), 1)
        ins = tdms_lite.channel(tdms_lite.read(data), "Pattern", "Instruction")
        self.assertEqual(ins[10], 0x40000008)
        self.assertEqual(p.set_count(10, 100000), (dp.COUNT_MAX, True))
        with self.assertRaises(ValueError):
            p.set_count(0, 3)                          # plain vector, no count


class PluginSim(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        shutil.copytree(PROJECT, os.path.join(self.tmp, "DemoPrj-TDC"))
        self.p = Tdc6571({"resource": "PXI1Slot15", "simulate": True,
                          "options": {"project": os.path.join(self.tmp, "DemoPrj-TDC"),
                                      "work_dir": os.path.join(self.tmp, "work"), "pattern": "TDC_M20_2"}})

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_pages(self):
        self.assertEqual(call(self.p, "info")["patterns"], 24)
        self.assertEqual(len(call(self.p, "list_patterns")["patterns"]), 24)
        s = call(self.p, "summary")
        self.assertEqual(s["hit_count"], 16)
        self.assertTrue(s["rows"] and s["events_rows"])
        tl = call(self.p, "timeline")
        self.assertEqual(len(tl["channels"]), 4)
        self.assertEqual(len(tl["t"]), len(tl["channels"][0]["data"]))
        self.assertEqual(len(call(self.p, "vectors")["rows"]), 21)
        self.assertTrue(call(self.p, "levels")["rows"])
        self.assertTrue(call(self.p, "timesets")["rows"])
        self.assertEqual(len(call(self.p, "pins")["rows"]), 14)

    def test_edit_save_and_select(self):
        rows = call(self.p, "counts")["rows"]
        hit_loop = next(r for r in rows if r["name"].startswith("v10"))
        self.assertEqual(hit_loop["value"], 16)
        call(self.p, "set_count", name="v10 set_loop", value=4)
        self.assertEqual(call(self.p, "summary")["hit_count"], 4)
        r = call(self.p, "save_as", name="TDC_M20_2_4hit")
        self.assertTrue(os.path.exists(r["saved"]))
        self.assertEqual(call(self.p, "info")["current"], "TDC_M20_2_4hit")
        s = call(self.p, "summary")
        self.assertEqual((s["hit_count"], s["modified"]), (4, False))
        self.assertEqual(s["label"], "TDC_M20_2")       # start label stays the template's

    def test_specs(self):
        call(self.p, "set_spec", name="DC.VDD", value=1.8)
        vdd = next(r for r in call(self.p, "specs")["rows"] if r["name"] == "DC.VDD")
        self.assertEqual((vdd["value"], vdd["unit"]), (1.8, "V"))
        i0 = next(r for r in call(self.p, "levels")["rows"] if r["pin"] == "I0")
        self.assertTrue(i0["vih"].startswith("1.8 V"))

    def test_burst_progress_and_abort(self):
        call(self.p, "select", name="TDC")             # 225 µs, no outer loop
        call(self.p, "burst")
        time.sleep(0.05)
        st = call(self.p, "status")
        self.assertEqual((st["state"], st["result"]), ("done", "PASS"))
        call(self.p, "select", name="TDC_M20_2")       # ~1.4 min
        st = call(self.p, "burst")
        self.assertEqual(st["state"], "running")
        with self.assertRaises(AssertionError):
            call(self.p, "burst")                      # already running
        st = call(self.p, "abort")
        self.assertEqual((st["state"], st["result"]), ("aborted", "ABORT"))
        call(self.p, "burst", single=True)             # one outer iteration ≈ 1.4 ms
        time.sleep(0.05)
        self.assertEqual(call(self.p, "status")["result"], "PASS")


class Generator(unittest.TestCase):
    PINS = ["I0", "I1", "I2", "I3", "start", "stop"]

    def build(self, **params):
        pl = tdcgen.plan(params, self.PINS)
        with open(os.path.join(PROJECT, "Patterns", "TDC_M20_2.digipat"), "rb") as fh:
            data = tdcgen.build(pl, fh.read())
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        path = os.path.join(tmp, pl["name"] + ".digipat")
        with open(path, "wb") as fh:
            fh.write(data)
        with open(tdcgen.sidecar_path(path), "w", encoding="utf-8") as fh:
            json.dump({"params": pl["params"], "roles": pl["roles"], "summary": tdcgen.summary(pl)}, fh)
        sheet = tdcgen.timing_sheet(pl, self.PINS, tmp)
        return pl, dp.Pattern(path), dp.load_timing(sheet), sheet

    def test_vectors_and_encoding(self):
        pl, p, timing, _ = self.build(name="GEN_T")
        self.assertEqual(p.name, "GEN_T")
        self.assertEqual(p.pins, ["I3", "I0", "I1", "I2"])            # CLK, TRIG, OUT1, OUT2
        self.assertEqual(p.timesets, [pl["timeset"]])
        i = p.label_names.index("WaitEdge")
        loc = p.label_locs[i]
        self.assertEqual(p.base_ins[loc], 0x70288000 | i)               # same wait word as WaitForBusy
        self.assertEqual(p.base_ins[loc - 1], 0x00200000)
        self.assertEqual(p.states[loc], "1H00")
        self.assertTrue(all(st[0] == "1" for st in p.states))           # CLK pulses in every vector
        self.assertEqual(p.instruction_text(len(p.base_ins) - 1), "halt")
        self.assertAlmostEqual(timing[pl["timeset"]]["period"], 20e-9)

    def test_timing_matches_request(self):
        pl, p, timing, _ = self.build(clock_mhz=100, out1_khz=1000, out1_cycles=3, out2_khz=250, out2_cycles=8,
                                      out2_delay_us=5, loops=4)
        a = dp.analyze(p, {k: v["period"] for k, v in timing.items()})
        self.assertEqual((a["start_pin"], a["hit_pin"], a["busy_pin"]), ("I1", "I2", "I0"))
        self.assertEqual((a["start_count"], a["hit_count"], a["loops"]), (3, 8, 4))
        self.assertAlmostEqual(a["start_period"], 1e-6, delta=1e-12)
        self.assertAlmostEqual(a["hit_period"], 4e-6, delta=1e-12)
        self.assertAlmostEqual(a["start_to_hit"], 5e-6, delta=1e-12)

    def test_falling_edge_and_quantisation(self):
        pl, p, _, _ = self.build(edge="falling", clock_mhz=100, out2_khz=300)
        i = p.label_names.index("WaitEdge")
        self.assertEqual(p.states[p.label_locs[i]][1], "L")
        self.assertTrue(pl["warnings"])                                  # 300 kHz is not a whole number of 10 ns

    def test_timing_sheet_has_rl_clock(self):
        import xml.etree.ElementTree as ET
        pl, _, _, sheet = self.build()
        root = ET.parse(sheet).getroot()
        edges = {e.get("pin"): [dp._local(c.tag) for c in e] for e in root.iter() if dp._local(e.tag) == "PinEdge"}
        self.assertIn("DriveReturnLow", edges["I3"])
        self.assertIn("DriveNonReturn", edges["I0"])
        self.assertEqual(set(edges), set(self.PINS))

    def test_rejects_bad_input(self):
        for bad in ({"out1_pin": "I0"}, {"name": "1abc"}, {"clock_mhz": 500}, {"out2_pin": "Q9"}, {"loops": 0}):
            with self.assertRaises(tdcgen.GenError):
                tdcgen.plan(bad, self.PINS)

    def test_plugin_generate_and_save(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        shutil.copytree(PROJECT, os.path.join(tmp, "P"))
        p = Tdc6571({"simulate": True, "options": {"project": os.path.join(tmp, "P"), "work_dir": os.path.join(tmp, "w")}})
        r = call(p, "generate", name="GEN_P", out2_cycles=4)
        self.assertEqual(r["name"], "GEN_P")
        s = call(p, "summary")
        self.assertEqual((s["pattern"], s["hit_count"], s["gen"]["vectors"]), ("GEN_P", 4, r["vectors"]))
        with self.assertRaises(AssertionError):
            call(p, "generate", name="TDC_M20_2")                          # would shadow a project pattern
        call(p, "burst")
        time.sleep(0.05)
        self.assertEqual(call(p, "status")["result"], "PASS")
        saved = call(p, "save_generated")
        self.assertTrue(os.path.exists(saved["saved"]) and os.path.exists(saved["timing"]))
        self.assertIn("GEN_P", [o["value"] for o in call(p, "list_patterns")["patterns"]])


if __name__ == "__main__":
    unittest.main()
