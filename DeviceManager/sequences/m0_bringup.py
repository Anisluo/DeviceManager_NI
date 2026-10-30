"""First-silicon bring-up of the Cortex-M0 test chip on the PXIe-6571.

1. Open/short (ESD diode) and leakage on every signal pin
2. SWD connect: IDCODE, CPUID, chip ID, AP IDR
3. Halt the core, SRAM march (write/read-back) through the AHB-AP
4. Bandgap trim loop: binary search of TRIM_BG for VREF = 1.200 V
   (simulator: the virtual chip's VREF; bench: replace measure_vref() with the 4081 DMM)

Every item becomes one row (item, value, low, high, unit, pass) of a "test" run,
so the data page / report shows a datasheet-style result table.
"""

dig = device("dig1")
SIM = dig.get_info()["backend"] == "sim"
new_run("M0 首片点亮", kind="test", dut="M0 test chip", board="6571 + 4145")

failures = []


def item(name, value, low=None, high=None, unit="", ok=None):
    if ok is None:
        ok = (low is None or value >= low) and (high is None or value <= high)
    record(item=name, value=value, low=low, high=high, unit=unit, passed=bool(ok))
    shown = (f"0x{value:08X}" if isinstance(value, int) and value > 0xFFFF
             else f"{value:.6g}" if isinstance(value, float) else value)
    print(f"{'PASS' if ok else 'FAIL'}  {name:28s} {shown} {unit}")
    if not ok:
        failures.append(name)
    return ok


# ---- 1. open / short + leakage ------------------------------------------------
progress(0.05, "开短路")
cont = dig.continuity(current=-100e-6, low=-0.95, high=-0.25)
for r in cont["rows"]:
    item(f"continuity {r['pin']}", r["voltage"], -0.95, -0.25, "V")
if not cont["pass"]:
    raise SystemExit(f"开短路失败：{cont['fails']} —— 先检查探针卡 / 插座，后续步骤跳过")

progress(0.2, "漏电")
leak = dig.leakage(limit=1e-6)
for r in leak["rows"]:
    item(f"leakage {r['pin']}", r["i_high"] if r["pull"] == "up" else r["i_low"], -1e-6, 1e-6, "A",
         ok=r["status"] == "ok")

# ---- 2. SWD -------------------------------------------------------------------------
progress(0.35, "SWD 连接")
dig.swd_configure(phy="static")            # bit-bang first: no pattern compiler involved
info = dig.swd_connect()
item("SWD IDCODE", info["idcode"], ok=info["idcode_hex"] == "0x0BB11477")
item("CPUID", int(info["cpuid_hex"], 16), ok=info["core"] == "Cortex-M0")
item("CHIP ID", int(info["chip_id_hex"], 16), ok=info["chip_id_hex"] != "—")

dig.swd_configure(phy="pattern", frequency=10e6)   # switch to full speed once the link works
info = dig.swd_connect()
item("SWD @10 MHz (pattern)", info["idcode"], ok=info["idcode_hex"] == "0x0BB11477")

# ---- 3. halt + SRAM march --------------------------------------------------------------
progress(0.5, "SRAM 测试")
st = dig.core_halt()
item("core halted", int(st["halted"]), ok=st["halted"])
BASE, N = 0x20000000, 256                  # first 1 KB
for pattern in (0x00000000, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555):
    dig.mem_write(address=hex(BASE), values=[pattern] * N, verify=False)
    words = dig.mem_read(address=hex(BASE), count=N)["words"]
    bad = sum(1 for w in words if w != pattern)
    item(f"SRAM march 0x{pattern:08X}", bad, 0, 0, "errors")
addr_test = [BASE + 4 * i for i in range(N)]
dig.mem_write(address=hex(BASE), values=addr_test, verify=False)
bad = sum(1 for a, w in zip(addr_test, dig.mem_read(address=hex(BASE), count=N)["words"]) if a != w)
item("SRAM address-in-data", bad, 0, 0, "errors")

# ---- 4. bandgap trim -----------------------------------------------------------------
progress(0.7, "基准 trim")
TRIM_BG = 0x40001000
TARGET = 1.200


def measure_vref():
    if SIM:
        return dig.sim_vref()["vref"]
    return device("dmm1").read()["value"]          # bench: 4081 on the VREF test point


lo, hi = 0, 31
before = measure_vref()
while lo < hi:                                      # VREF rises with the code
    mid = (lo + hi) // 2
    dig.mem_write(address=hex(TRIM_BG), value=mid)
    if measure_vref() < TARGET:
        lo = mid + 1
    else:
        hi = mid


def error_at(code):
    dig.mem_write(address=hex(TRIM_BG), value=code)
    return abs(measure_vref() - TARGET)


best = min({lo, max(lo - 1, 0)}, key=error_at)     # the search lands on the first code >= target; check its neighbour
dig.mem_write(address=hex(TRIM_BG), value=best)
after = measure_vref()
item("VREF untrimmed", before, unit="V", ok=True)
item("TRIM_BG code", best, 0, 31)
item("VREF trimmed", after, TARGET - 0.0015, TARGET + 0.0015, "V")

dig.core_resume()
progress(1.0, "完成")
print()
print("结果：" + ("全部通过" if not failures else f"{len(failures)} 项失败：{', '.join(failures)}"))
