"""Lint + simulate the platform RTL without Vivado: Yosys (yowasp-yosys from PyPI) and CXXRTL.

    pip install yowasp-yosys            # any Python 3.8+
    python run_sim.py [--cxx g++] [--out build_dir]

Steps: 1) read/elaborate/synthesise dm_platform_top (catches syntax errors, multiple
drivers, combinational loops), 2) convert dm_bridge_tb_top to C++ with CXXRTL,
3) compile tb_bridge.cpp against it and run the testbench.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RTL = os.path.dirname(HERE)


def yosys(script):
    from yowasp_yosys import run_yosys
    rc = run_yosys(["-q", "-p", script])
    if rc:
        raise SystemExit(f"yosys failed ({rc})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", default="g++")
    ap.add_argument("--out", default=os.path.join(RTL, "..", "..", "..", "build", "fpga_sim"))
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    os.chdir(RTL)
    fwd = lambda p: p.replace("\\", "/")

    print("1) lint + generic synthesis of dm_platform_top")
    yosys("read_verilog dm_uart.v dm_uart_bridge.v dm_ctrl_regs.v chip_top_stub.v dm_platform_top.v; "
          "hierarchy -check -top dm_platform_top; proc; check -assert; synth -top dm_platform_top")

    import yowasp_yosys
    runtime = os.path.join(os.path.dirname(yowasp_yosys.__file__), "share", "include", "backends", "cxxrtl", "runtime")
    benches = [
        ("UART bridge + registers", "dm_bridge_tb_top", "tb_top.cc", "tb_bridge",
         "dm_uart.v dm_uart_bridge.v dm_ctrl_regs.v sim/dm_bridge_tb_top.v"),
        ("AXI4-Lite adapter + registers", "dm_axil_tb_top", "axil_top.cc", "tb_axil",
         "dm_axil_adapter.v dm_ctrl_regs.v sim/dm_axil_tb_top.v"),
    ]
    failed = 0
    for i, (title, top, model, tb, files) in enumerate(benches, start=2):
        print(f"{i}) {title}: CXXRTL model, compile, run")
        yosys(f"read_verilog {files}; hierarchy -check -top {top}; proc; flatten; opt_clean; "
              f"write_cxxrtl {fwd(os.path.join(out, model))}")
        exe = os.path.join(out, tb + (".exe" if os.name == "nt" else ""))
        subprocess.check_call([a.cxx, "-std=c++17", "-O1", "-I", runtime, "-I", out,
                               os.path.join(HERE, tb + ".cpp"), "-o", exe])
        failed |= subprocess.call([exe])
    sys.exit(failed)


if __name__ == "__main__":
    main()
