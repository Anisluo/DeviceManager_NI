"""fpga_k7: UART protocol, simulated board, Vivado wrapper, and co-simulation with the 6571.
Run: python -m unittest discover -s devices/fpga_k7/tests"""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
ROOT = os.path.abspath(os.path.join(PLUGIN, "..", ".."))
sys.path[:0] = [PLUGIN, os.path.join(ROOT, "devices", "ni_digital"), os.path.join(ROOT, "sdk", "python")]

import uart_link  # noqa: E402
from fpga_k7 import FpgaK7  # noqa: E402
from sim_board import SimBoard  # noqa: E402


def call(p, name, **args):
    rc, text = p._dm_invoke_json(name, json.dumps(args))
    r = json.loads(text)
    if rc < 0:
        raise AssertionError(f"{name} -> {rc}: {r}")
    return r


def fpga(**sim):
    return FpgaK7({"resource": "K7", "simulate": True, "options": {"backend": "sim", "sim": sim}})


class Protocol(unittest.TestCase):
    def test_crc8_smbus_check_value(self):
        self.assertEqual(uart_link.crc8(b"123456789"), 0xF4)        # CRC-8/SMBUS catalogue value

    def test_frame_layout_matches_rtl(self):
        r = uart_link.encode_read(7, 0x100, 3)
        self.assertEqual(len(r), 9)
        self.assertEqual(r[:3], b"\xA5\x01\x07")
        w = uart_link.encode_write(8, 0x008, 0x12345678)
        self.assertEqual(len(w), 12)
        self.assertEqual(w[7:11], bytes([0x78, 0x56, 0x34, 0x12]))   # little endian

    def test_status_codes(self):
        b = SimBoard({})
        bad = bytearray(uart_link.encode_read(1, 0, 1))
        bad[-1] ^= 0xFF
        b.write(bytes(bad))
        resp = b.read(64, 0.1)
        self.assertEqual(resp[2], 0)          # N = 0
        self.assertEqual(resp[3], 1)          # STATUS = CRC error
        link = uart_link.UartLink(b, timeout=0.2, retries=0)
        b.write(b"\xA5\x07\x01\x00\x00\x00\x00\x01" + bytes([uart_link.crc8(b"\x07\x01\x00\x00\x00\x00\x01")]))
        self.assertEqual(b.read(64, 0.1)[3], 2)   # bad command
        self.assertEqual(link.read(0x000, 2), [0x444D4B37, 0x26102917])

    def test_unconfigured_board_times_out(self):
        link = uart_link.UartLink(SimBoard({"configured": False}), timeout=0.1, retries=0)
        with self.assertRaises(uart_link.LinkError):
            link.read(0)


class Plugin(unittest.TestCase):
    def test_ping_regs_clock(self):
        p = fpga()
        self.assertEqual(call(p, "ping")["id_hex"], "0x444D4B37")
        self.assertEqual(call(p, "reg_write", address="SCRATCH", value="0xCAFEF00D")["readback"], "0xCAFEF00D")
        rc, _ = p._dm_invoke_json("reg_write", json.dumps({"address": "ID", "value": 1}))
        self.assertLess(rc, 0)                                   # read-only
        st = call(p, "dut_clock", mhz=10)
        self.assertEqual(st["clk_div"], 9)
        self.assertAlmostEqual(st["dut_clock_hz"], 10e6)
        self.assertEqual(len(call(p, "reg_dump")["rows"]), 17)

    def test_jtag_program_and_xadc(self):
        p = fpga(configured=False)
        rc, _ = p._dm_invoke_json("ping", "{}")
        self.assertLess(rc, 0)                                   # not configured yet
        info = call(p, "program", bitstream="virtual_chip_sim.bit")
        self.assertEqual(info["done"], "1")
        self.assertTrue(info["ok"])
        self.assertTrue(20 < call(p, "xadc")["temperature"] < 60)


class CoSimulation(unittest.TestCase):
    """6571 simulator and FPGA platform simulator attached to one virtual chip."""

    def setUp(self):
        import dm.shared
        dm.shared.remove("k7chip_test")
        from ni_digital import NiDigital
        self.dig = NiDigital({"resource": "SIM", "options": {"backend": "sim", "vdd": 1.8,
                                                            "sim": {"seed": 3, "vdd": 1.8, "shared_dut": "k7chip_test"}}})
        self.k7 = fpga(shared_dut="k7chip_test")

    def test_reset_clock_and_swd(self):
        self.assertEqual(call(self.dig, "swd_connect")["idcode_hex"], "0x0BB11477")
        call(self.k7, "dut_reset", hold=True)
        self.assertFalse(call(self.dig, "swd_check")["pass"])        # chip held in reset
        call(self.k7, "dut_reset", hold=False)
        self.assertTrue(call(self.dig, "swd_check")["pass"])
        call(self.k7, "dut_clock", enable=False)
        self.assertFalse(call(self.dig, "swd_check")["pass"])
        call(self.k7, "dut_clock", enable=True)
        self.assertTrue(call(self.dig, "swd_check")["pass"])

    def test_swclk_edges_and_trim_mirror(self):
        e0 = call(self.k7, "swclk_edges")["edges"]
        call(self.dig, "swd_connect")
        self.assertGreater(call(self.k7, "swclk_edges")["edges"] - e0, 100)
        call(self.dig, "mem_write", address="0x40001000", value=0x13)      # TRIM_BG over SWD
        self.assertEqual(call(self.k7, "reg_read", address="TRIM_BG")["value"], 0x13)
        a = call(self.k7, "analog_model")
        self.assertAlmostEqual(a["vref"], call(self.dig, "sim_vref")["vref"], places=9)

    def test_power_on_reset_clears_trims(self):
        call(self.dig, "swd_connect")
        call(self.dig, "mem_write", address="0x40001000", value=0x05)
        call(self.k7, "dut_reset", hold=True)
        call(self.k7, "dut_reset", hold=False)
        self.assertEqual(call(self.k7, "reg_read", address="TRIM_BG")["value"], 0x10)


class VivadoWrapper(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bat = os.path.join(self.tmp.name, "vivado.bat")
        with open(self.bat, "w") as fh:
            fh.write(f'@"{sys.executable}" "{os.path.join(HERE, "fake_vivado.py")}"\r\n')

    def tearDown(self):
        self.tmp.cleanup()

    @unittest.skipUnless(os.name == "nt", "the fake Vivado is launched through a .bat")
    def test_session(self):
        import vivado
        v = vivado.Vivado(self.bat, startup_timeout=30)
        try:
            hw = vivado.HwSession(v)
            self.assertEqual(hw.device, "xc7k480t_0")
            info = hw.info()
            self.assertEqual(info["idcode"], "03656093")
            self.assertEqual(info["done"], "1")
            self.assertAlmostEqual(hw.xadc()["temperature"], 45.25)
            self.assertEqual(hw.axi_read(0), 0x444D4B37)
            with self.assertRaises(vivado.TclError) as cm:
                v.run("set_property BOGUS 1 [current_hw_device]")
            self.assertIn("Invalid option value", str(cm.exception))
            self.assertEqual(v.run("puts dm_ready"), "")                # still in sync after an error
        finally:
            v.close()


if __name__ == "__main__":
    unittest.main()
