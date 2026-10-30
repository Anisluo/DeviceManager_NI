"""ni_digital on the simulated backend: SWD engine, memory, core control, PPMU tests.
Run: python -m unittest discover -s devices/ni_digital/tests"""

import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.dirname(HERE), os.path.join(HERE, "..", "..", "..", "sdk", "python")]

import swd  # noqa: E402
from ni_digital import NiDigital  # noqa: E402
from sim_backend import SimBackend  # noqa: E402


def call(p, name, **args):
    rc, text = p._dm_invoke_json(name, json.dumps(args))
    r = json.loads(text)
    if rc < 0:
        raise AssertionError(f"{name} -> {rc}: {r}")
    return r


def plugin(**sim):
    return NiDigital({"resource": "SIM", "options": {"backend": "sim", "sim": dict({"seed": 1}, **sim)}})


class SwdProtocol(unittest.TestCase):
    def test_request_bits(self):
        # read DP IDCODE = 0xA5 (LSB first: 1 0 1 0 0 1 0 1)
        self.assertEqual(swd.request_bits(0, 1, 0), [1, 0, 1, 0, 0, 1, 0, 1])
        self.assertEqual(swd.from_bits(swd.request_bits(0, 1, 0)), 0xA5)

    def test_both_phys_read_idcode(self):
        for phy, freq in (("static", None), ("pattern", 4e6)):
            p = plugin()
            call(p, "swd_configure", phy=phy, **({"frequency": freq} if freq else {}))
            r = call(p, "swd_connect")
            self.assertEqual(r["idcode_hex"], "0x0BB11477", phy)
            self.assertEqual(r["core"], "Cortex-M0")
            self.assertEqual(r["ap_idr_hex"], "0x04770021")


class SwdMemoryAndCore(unittest.TestCase):
    def setUp(self):
        self.p = plugin()
        call(self.p, "swd_configure", phy="pattern", frequency=8e6)
        call(self.p, "swd_connect")

    def test_sram_roundtrip_across_1k_boundary(self):
        base = 0x200003F0                       # TAR auto-increment wraps at 0x20000400
        values = [0x11111111 * (i + 1) & 0xFFFFFFFF for i in range(8)]
        r = call(self.p, "mem_write", address=hex(base), values=values)
        self.assertTrue(r["verified"])
        self.assertEqual(call(self.p, "mem_read", address=hex(base), count=8)["words"], values)

    def test_flash_vector_table_and_fault(self):
        words = call(self.p, "mem_read", address="0x0", count=2)["words"]
        self.assertEqual(words, [0x20002000, 0x000000C1])
        rc, _ = self.p._dm_invoke_json("mem_read", json.dumps({"address": "0x30000000", "count": 1}))
        self.assertLess(rc, 0)                  # unmapped -> bus error -> sticky / FAULT
        call(self.p, "swd_connect")             # recovers after reconnect

    def test_halt_regs_reset(self):
        st = call(self.p, "core_halt")
        self.assertTrue(st["halted"])
        call(self.p, "core_write_reg", reg="r3", value="0xCAFEF00D")
        regs = {r["reg"]: r["dec"] for r in call(self.p, "core_regs")["rows"]}
        self.assertEqual(regs["r3"], 0xCAFEF00D)
        self.assertEqual(regs["sp"], 0x20002000)
        st = call(self.p, "core_reset", halt=True)
        self.assertTrue(st["halted"])
        regs = {r["reg"]: r["dec"] for r in call(self.p, "core_regs")["rows"]}
        self.assertEqual(regs["pc"], 0xC0)
        self.assertFalse(call(self.p, "core_resume")["halted"])

    def test_trim_registers(self):
        call(self.p, "mem_write", address="0x40001000", value=0x13)
        self.assertEqual(call(self.p, "mem_read", address="0x40001000", count=1)["words"], [0x13])
        self.assertAlmostEqual(call(self.p, "sim_vref")["vref"], 1.2 + 3 * 0.0025 + 0.011, places=6)


class Analog(unittest.TestCase):
    def test_continuity_detects_faults(self):
        p = plugin(faults={"PA1": "open", "PA2": "short_gnd", "PA3": "short:PA2"})
        r = call(p, "continuity")
        st = {row["pin"]: row["status"] for row in r["rows"]}
        self.assertEqual(st["SWDIO"], "ok")
        self.assertEqual(st["PA0"], "ok")
        self.assertEqual(st["PA1"], "open")
        self.assertEqual(st["PA2"], "short")
        self.assertEqual(st["PA3"], "short")
        self.assertFalse(r["pass"])
        v = {row["pin"]: row["voltage"] for row in r["rows"]}
        self.assertTrue(-0.9 < v["PA0"] < -0.4, v["PA0"])

    def test_leakage_and_pulls(self):
        p = plugin(faults={"PA0": "leaky"})
        r = call(p, "leakage", limit=1e-6)
        st = {row["pin"]: row for row in r["rows"]}
        self.assertEqual(st["PA0"]["status"], "fail")
        self.assertEqual(st["PA1"]["status"], "ok")
        self.assertEqual(st["SWDIO"]["status"], "ok")           # pull-up judged at VIH only
        self.assertLess(st["SWDIO"]["i_low"], -30e-6)           # pull-up (~ -82 µA) clamps at the 32 µA range
        self.assertTrue(st["SWDIO"].get("clamped"))

    def test_ppmu_force_current_clamps_on_open(self):
        p = plugin(faults={"PA1": "open"})
        r = call(p, "ppmu_force_current", pins="PA1", current=-10e-6, v_low=-1.0, v_high=1.0)
        self.assertLess(r["voltage"], -0.99)

    def test_swd_fails_with_open_swdio(self):
        p = plugin(faults={"SWDIO": "open"})
        rc, text = p._dm_invoke_json("swd_connect", "{}")
        self.assertLess(rc, 0)
        self.assertIn("SWD", json.loads(text)["error"])


class Shmoo(unittest.TestCase):
    def test_no_false_passes_below_threshold(self):
        p = plugin()
        call(p, "swd_configure", phy="pattern", frequency=2e6)
        call(p, "set_levels", pins="SWD", vih=2.1)                # undefined band: random bits
        self.assertEqual(sum(call(p, "swd_check")["pass"] for _ in range(40)), 0)

    def test_frequency_and_level_window(self):
        p = plugin()
        call(p, "swd_configure", phy="pattern", frequency=2e6)
        self.assertTrue(call(p, "swd_check")["pass"])
        call(p, "swd_configure", frequency=45e6)                # beyond fmax at 3.3 V
        self.assertFalse(call(p, "swd_check")["pass"])
        call(p, "swd_configure", frequency=2e6)
        call(p, "set_levels", pins="SWD", vih=2.0)               # below 0.7 * VDD
        self.assertFalse(call(p, "swd_check")["pass"])
        call(p, "sim_set_vdd", vdd=2.5)                          # now 2.0 V is a valid high
        self.assertTrue(call(p, "swd_check")["pass"])

DLL = os.path.join(HERE, "..", "..", "..", "build", "devices", "tdc_measure", "tdc_measure.dll")


@unittest.skipUnless(os.path.exists(DLL), "tdc_measure is not built")
class PatternProjectEditor(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        task = os.path.abspath(os.path.join(HERE, "..", "..", "tdc_measure"))
        self.p = NiDigital({"resource": "PXI1Slot15", "plugin_dir": self.tmp,
                            "uses": {"tdc_measure": {"library": os.path.abspath(DLL), "dir": task}},
                            "options": {"backend": "sim", "sim": {"seed": 1}}})

    def tearDown(self):
        self.p.close()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_edit_and_burst_through_the_driver(self):
        self.assertEqual(call(self.p, "pe.info")["current"], "main")
        call(self.p, "pe.edit_vector", v=0, field="pin:I0", value="1")
        self.assertTrue(call(self.p, "pe.vectors")["modified"])
        call(self.p, "pe.pattern_save")
        call(self.p, "pe.burst", single=False)                 # comes back to this driver
        self.assertIn(call(self.p, "pe.status")["state"], ("running", "done"))
        call(self.p, "pe.abort")
        rc, text = self.p._dm_invoke_json("pe.edit_vector", json.dumps({"v": 0, "field": "pin:I0", "value": "?"}))
        self.assertLess(rc, 0)
        self.assertIn("无效", json.loads(text)["error"])

    def test_large_results_are_fetched(self):
        self.assertTrue(call(self.p, "pe.timeline")["channels"])

    def test_without_the_library(self):
        p = NiDigital({"resource": "SIM", "options": {"backend": "sim", "sim": {"seed": 1}}})
        rc, text = p._dm_invoke_json("pe.info", "{}")
        self.assertLess(rc, 0)
        self.assertIn("tdc_measure", json.loads(text)["error"])
        p.close()


if __name__ == "__main__":
    unittest.main()


class DriverFiles(unittest.TestCase):
    """Offline checks of what the driver backend generates (no NI software needed)."""

    def test_pinmap_is_well_formed(self):
        import tempfile
        import xml.etree.ElementTree as ET
        from driver_backend import write_pinmap
        with tempfile.TemporaryDirectory() as d:
            path = write_pinmap(os.path.join(d, "t.pinmap"), "PXI1Slot15", {"SWCLK": 0, "SWDIO": 1})
            root = ET.parse(path).getroot()
        ns = {"p": "http://www.ni.com/TestStand/SemiconductorModule/PinMap.xsd"}
        conns = root.findall("p:Connections/p:Connection", ns)
        self.assertEqual([(c.get("pin"), c.get("channel")) for c in conns], [("SWCLK", "0"), ("SWDIO", "1")])

    def test_swd_pattern_source(self):
        from driver_backend import swd_pattern_source
        src = swd_pattern_source("swd_x", "wwtrrr")
        self.assertIn("pattern swd_x (SWCLK, SWDIO)", src)
        self.assertEqual(src.count(" 1 D;"), 2)
        self.assertEqual(src.count("capture  "), 3)
        self.assertIn("source_start(swd_src)", src)
        self.assertTrue(src.rstrip().endswith("}"))


class HostLoading(unittest.TestCase):
    """Load the plugin the way the host does: plugin folder on sys.path only during import."""

    @unittest.skipIf(__import__("importlib").util.find_spec("nidigital"), "nidigital installed: would open hardware")
    def test_driver_backend_importable_after_load(self):
        import types
        stub = types.ModuleType("_dmhost")
        stub.write = lambda stream, text: None
        sys.modules.setdefault("_dmhost", stub)
        plugin_dir = os.path.dirname(HERE)
        saved = list(sys.path)
        sys.path[:] = [p for p in sys.path if os.path.abspath(p) != os.path.abspath(plugin_dir)]
        for name in ("ni_digital", "swd", "driver_backend", "sim_backend", "sim_dut"):
            sys.modules.pop(name, None)
        try:
            from dm import _host
            cls = _host.load_plugin_class(os.path.join(plugin_dir, "ni_digital.py"), "dmplugin_test", "NiDigital")
            self.assertNotIn(plugin_dir, sys.path)
            with self.assertRaises(Exception) as cm:
                cls({"options": {"backend": "driver"}})
            self.assertNotIsInstance(cm.exception, ModuleNotFoundError)
            self.assertIn("nidigital", str(cm.exception))
            p = cls({"options": {"backend": "auto"}})      # falls back to the simulator
            self.assertEqual(p.backend.name, "sim")
        finally:
            sys.path[:] = saved
