"""Bench verification of the ni_digital plugin on a PXIe-8881 + PXIe-6571.

Runs the README checklist step by step without the GUI and writes everything
(versions, results, raw SWD bits, compiler output, tracebacks) to a log file.
Send the log back as-is.

    # 1) NI driver simulation — no hardware touched, checks every driver API call
    python verify_6571.py --simulate

    # 2) Real instrument, nothing on the pins (or DUT unpowered): instrument-only checks
    python verify_6571.py --resource PXI1Slot15 --no-dut

    # 3) Real instrument + powered DUT on the load board
    python verify_6571.py --resource PXI1Slot15 --pins SWCLK=0,SWDIO=1,nRST=2,PA0=4 --vdd 3.3

Needs: Python 3.x 64-bit, `pip install nidigital hightime`, NI-Digital Pattern Driver.
"""

import argparse
import datetime
import json
import os
import platform
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.dirname(HERE)
for p in (PLUGIN, os.path.join(PLUGIN, "..", "..", "sdk", "python")):
    sys.path.insert(0, os.path.abspath(p))

LOG = []
RESULTS = []

# Console code pages (cp936 …) cannot print µ/Ω: never let output encoding abort a step.
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass


def out(text=""):
    print(text)
    LOG.append(text)


def step(name, fn, expect_fail=False):
    out(f"\n=== {name}")
    t0 = time.time()
    try:
        detail = fn()
        status = "PASS"
    except Skip as s:
        status, detail = "SKIP", str(s)
    except Exception as e:  # noqa: BLE001
        status = "EXPECTED-FAIL" if expect_fail else "FAIL"
        detail = f"{type(e).__name__}: {e}"
        out(traceback.format_exc().rstrip())
    dt = time.time() - t0
    out(f"--> {status} ({dt:.2f} s) {detail if detail is not None else ''}")
    RESULTS.append((name, status, dt))
    return status == "PASS"


class Skip(Exception):
    pass


def call(plugin, method, **args):
    rc, text = plugin._dm_invoke_json(method, json.dumps(args))
    r = json.loads(text)
    if rc < 0:
        raise RuntimeError(f"{method} -> rc {rc}: {r.get('error')}")
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--resource", default="PXI1Slot15")
    ap.add_argument("--simulate", action="store_true", help="NI driver simulation (no hardware)")
    ap.add_argument("--no-dut", action="store_true", help="skip tests that need a DUT")
    ap.add_argument("--pins", help="NAME=channel,... (default: the plugin's default map)")
    ap.add_argument("--vdd", type=float, default=3.3)
    ap.add_argument("--idcode", default="0x0BB11477", help="expected SWD IDCODE")
    ap.add_argument("--compiler", help="path to DigitalPatternCompiler.exe")
    ap.add_argument("--log", help="log file (default: verify_6571_<time>.log in the current folder)")
    a = ap.parse_args()
    no_dut = a.no_dut or a.simulate

    out(f"verify_6571 · {datetime.datetime.now():%Y-%m-%d %H:%M:%S}")
    out(f"args: {vars(a)}")

    # ---- environment ---------------------------------------------------------
    def env():
        out(f"python   {sys.version}")
        out(f"platform {platform.platform()}")
        for mod in ("nidigital", "hightime", "numpy"):
            try:
                m = __import__(mod)
                out(f"{mod:9s}{getattr(m, '__version__', '?')}  ({os.path.dirname(m.__file__)})")
            except ImportError as e:
                out(f"{mod:9s}MISSING ({e})")
        import driver_backend
        out(f"compiler {driver_backend.find_compiler(a.compiler)}")
    step("0 environment", env)

    pins = None
    if a.pins:
        pins = {k.strip(): int(v) for k, v in (x.split("=") for x in a.pins.split(","))}
    options = {"backend": "driver", "vdd": a.vdd, "expect_idcode": a.idcode, "swd_phy": "static", "swd_frequency": 1e6}
    if pins:
        options["pins"] = pins
    if a.compiler:
        options["pattern_compiler"] = a.compiler

    from ni_digital import NiDigital
    holder = {}

    def open_session():
        holder["p"] = NiDigital({"resource": a.resource, "simulate": a.simulate, "options": options})
        p = holder["p"]
        out(f"pin map  {p.pin_map}")
        return call(p, "get_info")
    if not step("1 open session + pin map", open_session):
        return finish(a)
    p = holder["p"]
    b = p.backend
    all_pins = list(p.pin_map)

    # ---- instrument-only checks -------------------------------------------------
    def levels():
        call(p, "set_levels", pins="all", vil=0.0, vih=a.vdd, vol=0.3 * a.vdd, voh=0.7 * a.vdd, vterm=a.vdd / 2)
        call(p, "set_levels", pins=all_pins[0], vih=a.vdd * 0.9)
        r = call(p, "get_levels", pin=all_pins[0])
        out(f"readback {r}")
        assert abs(r["vih"] - a.vdd * 0.9) < 1e-3, "VIH did not read back"
        call(p, "set_levels", pins=all_pins[0], vih=a.vdd)
    step("2 levels set / read back", levels)

    def static():
        call(p, "write_static", pins="all", state="X")
        r = call(p, "read_static", pins="all")
        out(f"comparators (all X): {r['states']}")
        return None
    step("3 static write / read", static)

    def ppmu_order():
        # Force a different small voltage on every pin, read all back in one call:
        # verifies that ppmu_measure() returns values in pin order.
        forced = {pin: 0.02 * (i + 1) for i, pin in enumerate(all_pins)}
        for pin, v in forced.items():
            b.ppmu_configure([pin], "voltage", v, 2e-3)
        got = b.ppmu_measure(all_pins, "voltage")
        bad = []
        for pin, v in zip(all_pins, got):
            out(f"  {pin:10s} forced {forced[pin]:.3f} V  measured {v:.4f} V")
            if abs(v - forced[pin]) > 0.02:
                bad.append(pin)
        b.ppmu_off(all_pins)
        b.select_digital(all_pins)
        if bad and not a.simulate:
            raise AssertionError(f"order/accuracy mismatch on {bad}")
        return "order OK" if not bad else f"(simulated instrument returns placeholder values: {bad})"
    step("4 PPMU force-voltage, result order", ppmu_order)

    def ppmu_current():
        r = call(p, "ppmu_force_current", pins=all_pins[0], current=-10e-6, v_low=-1.5, v_high=1.0)
        out(f"  {all_pins[0]} -10 µA -> {r}")
        call(p, "ppmu_off", pins=all_pins[0])
    step("5 PPMU force-current", ppmu_current)

    # ---- pattern compiler ------------------------------------------------------------
    def compiler():
        import driver_backend
        src = os.path.join(b.work, "verify_probe.digipatsrc")
        dst = os.path.join(b.work, "verify_probe.digipat")
        if os.path.exists(dst):
            os.remove(dst)
        with open(src, "w", encoding="utf-8") as fh:
            fh.write(driver_backend.swd_pattern_source("verify_probe", "wwwwwwwwtrrr", capture_state=b.capture_state))
        out(open(src, encoding="utf-8").read())
        try:
            b.compile_pattern(src, dst)
        finally:
            for cmd, rc, text in b.compile_log:
                out(f"  $ {' '.join(cmd)}\n    -> {rc}\n    {text[:1500]}")
        b.s.load_pattern(dst)
        return f"compiled and loaded {dst}"
    compiled = step("6 pattern compiler (command line discovery)", compiler)

    # ---- DUT checks --------------------------------------------------------------------
    def dut(fn):
        def wrapper():
            if no_dut:
                raise Skip("no DUT (--no-dut / --simulate)")
            return fn()
        return wrapper

    def continuity():
        r = call(p, "continuity")
        for row in r["rows"]:
            out(f"  {row['pin']:10s} {row['voltage']:+.4f} V  {row['status']}")
        if not r["pass"]:
            raise AssertionError(f"fails: {r['fails']}")
    step("7 continuity", dut(continuity))

    def leakage():
        r = call(p, "leakage", v_high=a.vdd)
        for row in r["rows"]:
            out(f"  {row['pin']:10s} IIL {row['i_low']:+.3e}  IIH {row['i_high']:+.3e}  {row['pull']:5s} {row['status']}")
        if not r["pass"]:
            raise AssertionError(f"fails: {r['fails']}")
    step("8 leakage", dut(leakage))

    def raw_swd(phy_name):
        """Raw bits of a line reset + IDCODE read, for diagnosis."""
        import swd
        s = p._swd()
        try:
            s.line_reset()
        except Exception as e:  # noqa: BLE001
            out(f"  line reset raised {e}")
        head = swd.w(swd.request_bits(0, 1, 0)) + ["t", "r", "r", "r"]
        ack = s.phy.run(head)
        data = s.phy.run(["r"] * 33 + ["t", "w0", "w0"])
        out(f"  [{phy_name}] ACK bits (LSB first) {ack}  -> 0b{swd.from_bits(ack):03b} (OK = 0b001)")
        out(f"  [{phy_name}] data bits {''.join(map(str, data))}  -> 0x{swd.from_bits(data[:32]):08X}")

    def swd_static():
        call(p, "swd_configure", phy="static")
        try:
            r = call(p, "swd_connect")
        except Exception:
            out("  SWDIO idle comparator: " + str(call(p, "read_static", pins="SWDIO")))
            p.swd = None
            raw_swd("static")
            raise
        out(f"  {r}")
        if r["idcode_hex"].lower() != a.idcode.lower():
            raise AssertionError(f"IDCODE {r['idcode_hex']} != expected {a.idcode}")
    step("9 SWD static PHY connect", dut(swd_static))

    def swd_pattern():
        if not compiled:
            raise Skip("compiler step failed")
        tried = []
        for cs in ("X", "V"):
            b.capture_state = cs
            b.compiled.clear()
            call(p, "swd_configure", phy="pattern", frequency=1e6)
            try:
                r = call(p, "swd_connect")
                out(f"  capture_state {cs}: {r}")
                if r["idcode_hex"].lower() == a.idcode.lower():
                    return f"works with capture_state = {cs!r} → set options.capture_state if not 'X'"
                tried.append(f"{cs}: wrong IDCODE {r['idcode_hex']}")
            except Exception as e:  # noqa: BLE001
                tried.append(f"{cs}: {e}")
                p.swd = None
                try:
                    raw_swd(f"pattern/{cs}")
                except Exception as e2:  # noqa: BLE001
                    out(f"  raw capture raised {type(e2).__name__}: {e2}")
                    out(traceback.format_exc().rstrip())
        raise AssertionError("; ".join(tried))
    step("10 SWD pattern PHY connect (capture state X, then V)", dut(swd_pattern))

    def pattern_api_only():
        # In simulation / without a DUT: run one SWD-shaped burst to exercise the source/capture APIs.
        if not compiled:
            raise Skip("compiler step failed")
        if not no_dut:
            raise Skip("covered by step 10")
        bits = b.swd_cycles(["w1"] * 8 + ["t", "r", "r", "r"], 1e6)
        return f"burst + fetch_capture_waveform OK, captured {bits} (no DUT: values meaningless)"
    step("11 source/capture API without DUT", pattern_api_only)

    def freq_sweep():
        if not compiled:
            raise Skip("compiler step failed")
        res = []
        for f in (1e6, 2e6, 5e6, 10e6, 20e6, 30e6):
            call(p, "swd_configure", phy="pattern", frequency=f)
            ok = call(p, "swd_check")["pass"]
            res.append(f"{f / 1e6:g} MHz {'ok' if ok else 'FAIL'}")
            out("  " + res[-1])
        call(p, "swd_configure", phy="static", frequency=1e6)
        if res[0].endswith("FAIL"):
            raise AssertionError("fails already at 1 MHz: " + ", ".join(res))
        return ", ".join(res)
    step("12 SWCLK frequency sweep", dut(freq_sweep))

    try:
        p.close()
    except Exception:  # noqa: BLE001
        pass
    finish(a)


def finish(a):
    out("\n=== summary")
    for name, status, dt in RESULTS:
        out(f"  {status:13s} {name}  ({dt:.2f} s)")
    path = a.log or os.path.abspath(f"verify_6571_{datetime.datetime.now():%Y%m%d_%H%M%S}.log")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(LOG) + "\n")
    print(f"\nlog written to {path}")


if __name__ == "__main__":
    main()
