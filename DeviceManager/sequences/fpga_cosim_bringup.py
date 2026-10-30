"""Pre-silicon bring-up on the XC7K480T virtual chip, driven by the PXIe-6571 (1.8 V).

 1. FPGA: configured? (program the newest bitstream if not), platform ID / build, clock locked
 2. Release the virtual chip from reset, core clock 25 MHz
 3. 6571 → FPGA wiring: SWCLK edges counted by the FPGA while the 6571 runs SWD
 4. SWD: IDCODE (static PHY), then pattern PHY at 10 MHz
 5. Cross-check two paths: trim written over SWD (6571) == trim mirror read over UART (FPGA)
 6. Bandgap trim loop against the analog stub (VREF model fed by the RTL's TRIM_BG)
 7. Reset control: SWD must fail while the FPGA holds the chip in reset, recover after release
 8. Core clock sweep 1 … 50 MHz: SWD (clocked by SWCLK) must work at every core clock

Each item is a row of a "test" run (data page / report).
"""

fpga = device("fpga1")
dig = device("dig1")
new_run("K7 虚拟芯片联调点亮", kind="test", platform="XC7K480T", io="1.8 V")
fails = []


def item(name, value, ok, unit=""):
    record(item=name, value=value, unit=unit, passed=bool(ok))
    shown = f"{value:.6g}" if isinstance(value, float) else value
    print(f"{'PASS' if ok else 'FAIL'}  {name:34s} {shown} {unit}")
    if not ok:
        fails.append(name)
    return ok


# 1 ---------------------------------------------------------------------------
progress(0.05, "FPGA 链路")
try:
    p = fpga.ping()
except DeviceError:
    print("平台寄存器无响应，下载最新 bitstream …")
    p = fpga.program()
item("platform ID", p["id_hex"], p["id_hex"] == "0x444D4B37")
item("build", p["build_hex"], True)
st = fpga.status()
item("clock locked", int(st["clk_locked"]), st["clk_locked"])

# 2 ---------------------------------------------------------------------------
fpga.dut_reset(hold=False)
st = fpga.dut_clock(enable=True, mhz=25)
item("DUT clock", st["dut_clock_hz"], abs(st["dut_clock_hz"] - 25e6) < 1e6, "Hz")
item("DUT running", int(st["dut_running"]), st["dut_running"])

# 3 ---------------------------------------------------------------------------
progress(0.25, "6571 → FPGA 连线")
dig.set_levels(pins="all", vil=0.0, vih=1.8, vol=0.54, voh=1.26, vterm=0.9)
e0 = fpga.swclk_edges()["edges"]
dig.swd_configure(phy="static")
ok = dig.swd_check()["pass"]
edges = fpga.swclk_edges()["edges"] - e0
item("SWCLK edges seen by FPGA", edges, edges >= 100, "edges")

# 4 ---------------------------------------------------------------------------
progress(0.4, "SWD")
info = dig.swd_connect()
item("SWD IDCODE (static)", info["idcode_hex"], info["idcode_hex"] == "0x0BB11477")
dig.swd_configure(phy="pattern", frequency=10e6)
info = dig.swd_connect()
item("SWD IDCODE (pattern 10 MHz)", info["idcode_hex"], info["idcode_hex"] == "0x0BB11477")

# 5 ---------------------------------------------------------------------------
progress(0.5, "SWD / UART 交叉校验")
for code in (0x00, 0x1F, 0x0A):
    dig.mem_write(address="0x40001000", value=code)
    mirror = fpga.reg_read(address="TRIM_BG")["value"]
    item(f"TRIM_BG 0x{code:02X}: SWD → UART", mirror, mirror == code)

# 6 ---------------------------------------------------------------------------
progress(0.6, "基准 trim（模拟量桩）")
TARGET = 1.200


def vref_at(code):
    dig.mem_write(address="0x40001000", value=code)
    return fpga.analog_model()["vref"]


lo, hi = 0, 31
while lo < hi:
    mid = (lo + hi) // 2
    if vref_at(mid) < TARGET:
        lo = mid + 1
    else:
        hi = mid
best = min({lo, max(lo - 1, 0)}, key=lambda c: abs(vref_at(c) - TARGET))
v = vref_at(best)
item("TRIM_BG code", best, 0 <= best <= 31)
item("VREF trimmed (stub)", v, abs(v - TARGET) <= 0.0015, "V")

# 7 ---------------------------------------------------------------------------
progress(0.75, "复位控制")
fpga.dut_reset(hold=True)
item("SWD blocked while in reset", int(not dig.swd_check()["pass"]), not dig.swd_check()["pass"])
fpga.dut_reset(hold=False)
item("SWD after reset release", int(dig.swd_check()["pass"]), dig.swd_check()["pass"])
item("trim back to default after POR", fpga.reg_read(address="TRIM_BG")["value"],
     fpga.reg_read(address="TRIM_BG")["value"] == 0x10)

# 8 ---------------------------------------------------------------------------
for i, mhz in enumerate((1, 5, 10, 25, 50)):
    progress(0.8 + 0.04 * i, f"内核时钟 {mhz} MHz")
    fpga.dut_clock(mhz=mhz)
    item(f"SWD @ core {mhz} MHz", int(dig.swd_check()["pass"]), dig.swd_check()["pass"])

fpga.dut_clock(mhz=25)
dig.swd_configure(phy="static", frequency=1e6)
progress(1.0, "完成")
print()
print("结果：" + ("全部通过" if not fails else f"{len(fails)} 项失败：{', '.join(fails)}"))
