"""Sample generated wire signals independently of their transaction annotations."""
import bisect
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "sdk/python"), str(Path(__file__).resolve().parents[1])]
from protocol_sim import ProtocolSim, bits, crc15


def at(lane, t):
    return lane["v"][bisect.bisect_right(lane["t"], t)-1]


def lanes(cap):
    return {l["name"]: l for l in cap["lanes"]}


def sample(lane, start, count, period):
    return [at(lane, start+(i+.5)*period) for i in range(count)]


class Protocols(unittest.TestCase):
    def setUp(self):
        self.p = ProtocolSim({})

    def test_defaults_and_all_traces_ordered(self):
        for protocol in ("SWD", "SPI", "I2C", "PCIE", "AXI", "UART", "CAN"):
            with self.subTest(protocol=protocol):
                cap = self.p.capture(protocol)
                self.assertGreater(cap["t_end"], 0)
                self.assertGreater(cap["event_count"], 0)
                for lane in cap["lanes"]:
                    self.assertEqual(len(lane["t"]), len(lane["v"]))
                    self.assertEqual(lane["t"], sorted(set(lane["t"])))
                    self.assertLessEqual(lane["t"][-1], cap["t_end"])
                self.assertIs(self.p.capture(protocol), cap)

    def test_spi_all_modes_sample_at_correct_edges(self):
        for mode in range(4):
            for order in ("MSB", "LSB"):
                cap = self.p.generate("SPI", 1000, {"mode": mode, "order": order, "data": "A5 12", "response": "3C 81"})
                ls = lanes(cap); clock = ls["SCLK"]
                polarity, phase = mode >> 1, mode & 1
                self.assertEqual(clock["v"][0], polarity)
                sample_level = polarity if phase else 1-polarity
                times = [t for t, v in zip(clock["t"][1:], clock["v"][1:]) if v == sample_level]
                self.assertEqual(len(times), 16)
                for name, values in (("MOSI", [0xA5, 0x12]), ("MISO", [0x3C, 0x81])):
                    expected = [b for v in values for b in bits(v, 8, order == "LSB")]
                    self.assertEqual([at(ls[name], t-1e-12) for t in times], expected)
                self.assertEqual(ls["CS#"]["v"][-1], 1)

    def test_uart_start_data_parity_and_stop(self):
        cap = self.p.generate("UART", 1000, {"data": "53", "parity": "even", "stop_bits": 2})
        seq = sample(lanes(cap)["TX"], .002, 12, .001)
        self.assertEqual(seq, [0] + bits(0x53, 8, True) + [0, 1, 1])
        bad = self.p.generate("UART", 1000, {"data": "53", "parity": "even", "fault": "parity"})
        self.assertEqual(at(lanes(bad)["TX"], .0115), 1)
        self.assertEqual(bad["error_count"], 1)

    def test_bit_labels_reconstruct_bytes_and_align_to_sampling_edges(self):
        for mode in range(4):
            for order in ("MSB", "LSB"):
                cap = self.p.generate("SPI", 1000, {"mode": mode, "order": order, "data": "9F", "response": "EF"})
                ls = lanes(cap)
                self.assertEqual([l["name"] for l in cap["lanes"] if l["show_edges"]], ["SCLK"])
                expected_level = (mode >> 1) if mode & 1 else 1-(mode >> 1)
                self.assertTrue(all(at(ls["SCLK"], t+1e-12) == expected_level for t in ls["SCLK"]["samples"]))
                self.assertEqual(ls["SCLK"]["samples"], [b["sample"] for b in ls["MOSI"]["bits"]])
                for name, expected in (("MOSI", 0x9F), ("MISO", 0xEF)):
                    markers = ls[name]["bits"]
                    self.assertEqual(len(markers), 8)
                    reconstructed = sum(b["value"] << int(b["label"][1:]) for b in markers)
                    self.assertEqual(reconstructed, expected)
                    self.assertEqual([b["label"] for b in markers], [f"D{i}" for i in (range(8) if order == "LSB" else reversed(range(8)))])
                    for b in markers:
                        self.assertEqual(at(ls[name], b["sample"]-1e-12), b["value"])
                        self.assertAlmostEqual(at(ls["SCLK"], b["sample"]-1e-12), 1-at(ls["SCLK"], b["sample"]+1e-12))
        cap = self.p.generate("I2C")
        self.assertEqual([b["label"] for b in lanes(cap)["SDA"]["bits"][:8]], ["A6", "A5", "A4", "A3", "A2", "A1", "A0", "R/W"])

    def test_i2c_address_data_ack_stop_and_stretch(self):
        cap = self.p.generate("I2C", 1000, {"address": 0x50, "data": "A5", "stretch": 2})
        ls = lanes(cap)
        rising = [t for t, v in zip(ls["SCL"]["t"][1:], ls["SCL"]["v"][1:]) if v == 1]
        seq = [at(ls["SDA"], t-1e-12) for t in rising[:-1]]  # last SCL rise belongs to STOP
        self.assertEqual(seq, bits(0xA0, 8)+[0]+bits(0xA5, 8)+[0])
        self.assertEqual(ls["SCL"]["v"][-1], 1)
        self.assertEqual(ls["SDA"]["v"][-1], 1)
        normal = self.p.generate("I2C", 1000, {"address": 0x50, "data": "A5"})
        self.assertAlmostEqual(cap["t_end"]-normal["t_end"], .002)
        read = self.p.generate("I2C", options={"operation": "read", "data": "12 34"})
        self.assertEqual([e["label"] for e in read["events"]][-2:], ["NACK", "STOP"])
        nack = self.p.generate("I2C", options={"data": "12 34", "nack_at": 0})
        self.assertFalse(any(e["label"].startswith("DATA") for e in nack["events"]))

    def test_swd_request_ack_turnaround_and_parity(self):
        cap = self.p.generate("SWD", 1000, {"line_reset": 0, "value": 0x2BA01477})
        req = next(e for e in cap["events"] if e["label"] == "REQUEST")
        self.assertEqual(sample(lanes(cap)["SWDIO"], req["t"], 8, .001), bits(0xA5, 8, True))
        data = next(e for e in cap["events"] if e["label"].startswith("DATA"))
        self.assertEqual(sample(lanes(cap)["SWDIO"], data["t"], 32, .001), bits(0x2BA01477, 32, True))
        self.assertEqual(at(lanes(cap)["HOST_OE"], data["t"]+.001), 0)
        for ack in ("WAIT", "FAULT"):
            cap = self.p.generate("SWD", options={"ack": ack})
            self.assertFalse(any(e["label"].startswith("DATA") for e in cap["events"]))

    def test_can_crc_known_check_and_destuff(self):
        self.assertEqual(crc15([b for v in b"123456789" for b in bits(v, 8)]), 0x059E)
        cap = self.p.generate("CAN", 1000, {"identifier": 0, "data": "00 00"})
        eof = next(e for e in cap["events"] if e["label"] == "CRC delimiter")
        stream = sample(lanes(cap)["CAN"], .003, round((eof["t"]-.003)/.001), .001)
        raw = []; last = None; run = 0; skip = False
        for bit in stream:
            if skip:
                self.assertNotEqual(bit, last)
                last, run, skip = bit, 1, False
                continue
            raw.append(bit)
            run = run+1 if bit == last else 1; last = bit
            skip = run == 5
        self.assertEqual(crc15(raw[:-15]), int("".join(map(str, raw[-15:])), 2))
        self.assertEqual(raw[15:19], [0, 0, 1, 0])  # DLC=2
        ack = next(e for e in cap["events"] if e["label"] == "ACK")
        self.assertEqual(at(lanes(cap)["TXD"], ack["t"]+.0005), 1)
        self.assertEqual(at(lanes(cap)["RXD"], ack["t"]+.0005), 0)

    def test_axi_handshake_stability_and_last(self):
        for op in ("read", "write"):
            cap = self.p.generate("AXI", 1000, {"operation": op, "stall": 3, "data": "12345678 89ABCDEF"})
            ls = lanes(cap); d = "W" if op == "write" else "R"
            samples = [t for t, v in zip(ls["ACLK"]["t"], ls["ACLK"]["v"]) if v == 1]
            transfers = [t for t in samples if at(ls[d+"VALID"], t) and at(ls[d+"READY"], t)]
            self.assertEqual([at(ls[d+"DATA"], t) for t in transfers], [0x12345678, 0x89ABCDEF])
            self.assertEqual([at(ls[d+"LAST"], t) for t in transfers], [0, 1])
            for t in samples:
                if at(ls[d+"VALID"], t) and not at(ls[d+"READY"], t):
                    self.assertEqual(at(ls[d+"DATA"], t), at(ls[d+"DATA"], t+.001))

    def test_pcie_transaction_packet_duration(self):
        read = self.p.generate("PCIE", 1000, {"operation": "MRd", "latency": 5})
        write = self.p.generate("PCIE", 1000, {"operation": "MWr"})
        self.assertEqual(len(read["events"]), 2)
        self.assertAlmostEqual(read["events"][1]["t"]-read["events"][0]["end"], .005)
        self.assertAlmostEqual(write["events"][0]["duration"], .004)
        self.assertEqual(len(write["events"]), 1)
        self.assertIn("不是物理串行波形", read["description"])

    def test_exports_keep_bus_widths_and_do_not_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.p.generate("AXI")
            path = str(Path(tmp) / "capture.vcd")
            self.p.export("AXI", path)
            data = Path(path).read_text()
            self.assertIn("wire 32", data)
            self.assertIn("00010010001101000101011001111000", data)
            with self.assertRaises(FileExistsError): self.p.export("AXI", path)
            other = str(Path(tmp) / "capture.json")
            self.p.export("AXI", other, "json")
            self.assertEqual(json.loads(Path(other).read_text(encoding="utf-8"))["protocol"], "AXI")

    def test_invalid_inputs_leave_previous_capture_intact(self):
        old = self.p.generate("SPI")
        invalid = [("SPI", {"data": "100", "response": "00"}), ("SPI", {"mode": 4}),
                   ("I2C", {"address": 128}), ("SWD", {"address": 1}), ("UART", {"fault": "parity"}),
                   ("AXI", {"address": "0xFFC"}), ("CAN", {"data": "00 "*9}), ("PCIE", {"data": "11"})]
        for protocol, options in invalid:
            with self.subTest(protocol=protocol, options=options):
                rc, result = self.p._dm_invoke_json("simulate", json.dumps({"protocol": protocol, "options": options}))
                self.assertLess(rc, 0, result)
        self.assertIs(self.p.capture("SPI"), old)
        for bad in (0, -1, float("nan"), float("inf")):
            with self.assertRaises(ValueError): self.p.generate("SPI", bad)

    def test_page_actions_match_plugin_and_defaults(self):
        page = json.loads((Path(__file__).resolve().parents[1]/"page.json").read_text(encoding="utf-8"))
        def substitute(value, form):
            if isinstance(value, str) and value.startswith("$form."): return form[value[6:]]
            if isinstance(value, dict): return {k: substitute(v, form) for k, v in value.items()}
            return value
        for tab in page["tabs"]:
            widgets = [w for sec in tab["sections"] for w in sec["widgets"]]
            form = {w["id"]: w["value"] for w in widgets if "value" in w}
            button = next(w for w in widgets if w.get("action", {}).get("method") == "simulate")
            args = substitute(button["action"]["args"], form)
            rc, value = self.p._dm_invoke_json("simulate", json.dumps(args))
            self.assertEqual(rc, 0, value)


if __name__ == "__main__": unittest.main()
