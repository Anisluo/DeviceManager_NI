"""VDD × SWCLK shmoo of the SWD link (6571 pattern PHY).

On the bench VDD comes from the PXIe-4145 (smu1 ch1) and the 6571 drive levels
track it; in the simulator the virtual chip's VDD is set directly.
"""

dig = device("dig1")
SIM = dig.get_info()["backend"] == "sim"
smu = device("smu1")


def set_vdd(v):
    if SIM:
        dig.sim_set_vdd(vdd=v)
    else:
        smu.set_voltage(channel=1, voltage=v)
        sleep(0.005)
    dig.set_levels(pins="SWD", vih=v, voh=0.7 * v, vol=0.3 * v)   # 6571 levels follow VDD


result = shmoo(
    "SWD VDD × SWCLK",
    x=Axis("fswd", [f * 1e6 for f in range(2, 42, 2)], set=lambda f: dig.swd_configure(phy="pattern", frequency=f), unit="Hz"),
    y=Axis("vdd", sweep(1.5, 3.6, 0.1), set=set_vdd, unit="V"),
    test=lambda p: dig.swd_check()["pass"],
    setup=lambda: dig.swd_configure(phy="pattern"),
    teardown=lambda: (set_vdd(3.3), dig.swd_configure(phy="static", frequency=1e6)),
)

passed = sum(1 for r in result.rows if r["pass"])
print(f"{passed}/{len(result)} 通过")
for vdd in (1.8, 2.5, 3.3):
    ok = [r["fswd"] for r in result.rows if abs(r["vdd"] - vdd) < 1e-9 and r["pass"]]
    print(f"VDD = {vdd:.1f} V : 最高通过 SWCLK = {max(ok) / 1e6:.0f} MHz" if ok else f"VDD = {vdd:.1f} V : 无通过点")
