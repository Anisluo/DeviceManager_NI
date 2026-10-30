"""Stands in for `vivado -mode tcl`: understands the wrapper vivado.Vivado sends and
answers a few hardware-manager commands. For unit tests only."""

import re
import sys

print("****** Vivado v2025.1 (fake)", flush=True)
for line in sys.stdin:
    m = re.match(r"if \{\[catch \{(.*)\} __dm_r\]\} \{puts \"@@DM_ERR (\w+) ", line)
    if not m:
        if line.strip() == "exit":
            break
        continue
    cmd, tok = m.group(1), m.group(2)
    print(f"# {cmd[:60]}", flush=True)                         # noise the wrapper must skip
    if "BOGUS" in cmd:
        print(f"@@DM_ERR {tok} ERROR: [Common 17-161] Invalid option value 'BOGUS'", flush=True)
        continue
    result = ""
    if cmd.startswith("puts dm_ready"):
        print("dm_ready", flush=True)
    elif "get_hw_targets" in cmd:
        result = "localhost:3121/xilinx_tcf/Digilent/210249B0C123"
    elif "get_hw_devices" in cmd:
        result = "xc7k480t_0"
    elif "IDCODE_HEX" in cmd:
        result = "03656093"
    elif "PROPERTY PART" in cmd.upper() or cmd.endswith("PART [current_hw_device]"):
        result = "xc7k480tffg901-3"
    elif "DONE_PIN" in cmd:
        result = "1"
    elif "get_hw_sysmons" in cmd:
        result = "45.25 1.002 1.797 1.001"
    elif "get_hw_axi_txns dm_rd" in cmd:
        result = "444D4B37"
    print(f"@@DM_OK {tok} {result}", flush=True)
