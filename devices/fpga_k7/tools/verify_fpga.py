"""Bench verification of the fpga_k7 plugin on the PXIe-8881 + XC7K480T board.

Writes a log with every result, Vivado / Tcl message and traceback — send it back as-is.

    python verify_fpga.py --list-ports                       # which COM port is the board?
    python verify_fpga.py --port COM3                         # UART register bridge only
    python verify_fpga.py --port COM3 --jtag                  # + Vivado: device info, XADC
    python verify_fpga.py --port COM3 --jtag --bitstream D:/fpga/bitstreams/platform.bit
    python verify_fpga.py --jtag --jtag-axi                   # registers over JTAG-to-AXI instead of UART

Needs: pip install pyserial; Vivado or Vivado Lab with hw_server for --jtag.
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
for stream in (sys.stdout, sys.stderr):
    try:
        stream.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

LOG, RESULTS = [], []


def out(text=""):
    print(text)
    LOG.append(str(text))


class Skip(Exception):
    pass


def step(name, fn):
    out(f"\n=== {name}")
    t0 = time.time()
    try:
        detail, status = fn(), "PASS"
    except Skip as s:
        detail, status = str(s), "SKIP"
    except Exception as e:  # noqa: BLE001
        detail, status = f"{type(e).__name__}: {e}", "FAIL"
        out(traceback.format_exc().rstrip())
    out(f"--> {status} ({time.time() - t0:.2f} s) {detail if detail is not None else ''}")
    RESULTS.append((name, status))
    return status == "PASS"


def call(p, method, **args):
    rc, text = p._dm_invoke_json(method, json.dumps(args))
    r = json.loads(text)
    if rc < 0:
        raise RuntimeError(f"{method} -> rc {rc}: {r.get('error')}")
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="COM port of the board's USB-UART")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--list-ports", action="store_true")
    ap.add_argument("--jtag", action="store_true", help="also test Vivado / hw_server")
    ap.add_argument("--jtag-axi", action="store_true", help="register access over JTAG-to-AXI")
    ap.add_argument("--vivado", help="path to vivado.bat / vivado_lab.bat")
    ap.add_argument("--hw-server", default="localhost:3121")
    ap.add_argument("--bitstream", help="program this .bit first")
    ap.add_argument("--log")
    a = ap.parse_args()
    out(f"verify_fpga · {datetime.datetime.now():%Y-%m-%d %H:%M:%S} · args {vars(a)}")

    def env():
        out(f"python   {sys.version}")
        out(f"platform {platform.platform()}")
        try:
            import serial
            from serial.tools import list_ports
            out(f"pyserial {serial.__version__}")
            for port in list_ports.comports():
                out(f"  {port.device:8s} {port.description}  [{port.hwid}]")
        except ImportError as e:
            out(f"pyserial MISSING ({e})")
        import vivado
        out(f"vivado   {vivado.find_vivado(a.vivado)}")
    step("0 environment / COM ports / Vivado", env)
    if a.list_ports:
        return finish(a)

    options = {"backend": "hw", "hw_server": a.hw_server}
    if a.vivado:
        options["vivado"] = a.vivado
    if a.jtag_axi:
        options["reg_access"] = "jtag"
    elif a.port:
        options.update({"reg_access": "uart", "uart_port": a.port, "baud": a.baud})
    else:
        options["reg_access"] = "jtag"
    if a.bitstream:
        options["bitstream_dir"] = os.path.dirname(os.path.abspath(a.bitstream))

    from fpga_k7 import FpgaK7
    holder = {}

    def open_plugin():
        holder["p"] = FpgaK7({"resource": "XC7K480T", "simulate": False, "options": options})
        return f"register access: {holder['p'].access}"
    if not step("1 open plugin", open_plugin):
        return finish(a)
    p = holder["p"]

    def jtag():
        if not (a.jtag or a.jtag_axi or a.bitstream):
            raise Skip("no --jtag")
        t0 = time.time()
        r = call(p, "jtag_connect")
        out(f"  Vivado start + open target: {time.time() - t0:.1f} s")
        out(f"  {r}")
        for k in ("idcode", "done", "part"):
            if str(r.get(k, "")).startswith("?"):
                out(f"  NOTE: property for '{k}' not available: {r[k]}")
        return r.get("device")
    have_jtag = step("2 JTAG connect (Vivado hw_server)", jtag)

    def program():
        if not a.bitstream:
            raise Skip("no --bitstream")
        if not have_jtag:
            raise Skip("JTAG step failed")
        r = call(p, "program", bitstream=os.path.basename(a.bitstream))
        out(f"  {r}")
        return f"{r.get('program_s', 0):.1f} s, DONE = {r.get('done')}"
    step("3 program bitstream", program)

    def xadc():
        if not have_jtag:
            raise Skip("no JTAG")
        return call(p, "xadc")
    step("4 XADC", xadc)

    def ping():
        r = call(p, "ping")
        return f"ID {r['id_hex']}, BUILD {r['build_hex']}, {r['latency_ms']:.2f} ms"
    linked = step("5 register link: ping (ID / BUILD)", ping)

    def scratch():
        if not linked:
            raise Skip("no link")
        bad = []
        for v in [0, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555] + [1 << i for i in range(32)]:
            got = call(p, "reg_write", address="SCRATCH", value=hex(v))["readback"]
            if int(got, 16) != v:
                bad.append((hex(v), got))
        if bad:
            raise AssertionError(f"mismatches: {bad[:5]}")
        return "36 patterns OK"
    step("6 SCRATCH walking ones / patterns", scratch)

    def throughput():
        if not linked:
            raise Skip("no link")
        t0 = time.time()
        n = 0
        while time.time() - t0 < 1.0:
            call(p, "reg_read", address="0x000", count=64)
            n += 64
        return f"{n / (time.time() - t0):.0f} words/s"
    step("7 read throughput", throughput)

    def dut_control():
        if not linked:
            raise Skip("no link")
        s1 = call(p, "dut_reset", hold=True)
        s2 = call(p, "dut_reset", hold=False)
        s3 = call(p, "dut_clock", mhz=25)
        out(f"  hold: running={s1['dut_running']}  release: running={s2['dut_running']}  clock {s3['dut_clock_hz'] / 1e6:.2f} MHz")
        if s1["dut_running"] or not s2["dut_running"]:
            raise AssertionError("STATUS.dut_running does not follow CTRL")
        return call(p, "status")
    step("8 DUT reset / clock control", dut_control)

    def regs():
        if not linked:
            raise Skip("no link")
        for r in call(p, "reg_dump")["rows"]:
            out(f"  {r['address']} {r['name']:12s} {r['value']}")
    step("9 register dump", regs)

    def swclk():
        if not linked:
            raise Skip("no link")
        e0 = call(p, "swclk_edges")["edges"]
        out("  (run any SWD access on the 6571 within 5 s to see edges increase)")
        time.sleep(5)
        return f"SWCLK edges +{call(p, 'swclk_edges')['edges'] - e0}"
    step("10 SWCLK edges from the 6571", swclk)

    p.close()
    finish(a)


def finish(a):
    out("\n=== summary")
    for name, status in RESULTS:
        out(f"  {status:5s} {name}")
    path = a.log or os.path.abspath(f"verify_fpga_{datetime.datetime.now():%Y%m%d_%H%M%S}.log")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(LOG) + "\n")
    print(f"\nlog written to {path}")


if __name__ == "__main__":
    main()
