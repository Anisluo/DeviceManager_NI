"""LDO line regulation / dropout sweep.

smu1 ch0 sources VIN into the LDO (sim_smu "ldo" load model) and measures IIN;
dmm1 reads VOUT through its probe. Each point is recorded, so the data tab
plots VOUT(VIN) and the console prints the extracted figures at the end.
"""

VOUT_NOM = 3.3
RLOAD = 330.0                     # ≈ 10 mA load
VIN_POINTS = sweep(3.0, 5.5, 0.05)

smu = device("smu1")
dmm = device("dmm1")

smu.reset()
smu.set_load(channel=0, load="ldo", r_load=RLOAD, ldo_vout=VOUT_NOM)
smu.set_current_limit(channel=0, current_limit=0.05)
dmm.configure(function="dcv", range=VOUT_NOM, nplc=1)
smu.set_voltage(channel=0, voltage=VIN_POINTS[0])
smu.set_output(channel=0, enabled=True)
sleep(0.05)

rows = []
try:
    for i, vin in enumerate(VIN_POINTS):
        smu.set_voltage(channel=0, voltage=vin)
        sleep(0.01)                                   # settle
        iin = smu.measure(channel=0)["current"]
        vout = dmm.read()["value"]
        record(vin=vin, vout=vout, iin=iin)
        rows.append((vin, vout, iin))
        progress((i + 1) / len(VIN_POINTS), f"VIN = {vin:.2f} V")
finally:
    smu.set_output(channel=0, enabled=False)          # always leave the DUT unpowered

# ---- figures of merit ---------------------------------------------------
in_reg = [(vin, vout) for vin, vout, _ in rows if vin >= VOUT_NOM + 0.5]
(v1, o1), (v2, o2) = in_reg[0], in_reg[-1]
line_reg = (o2 - o1) / o1 / (v2 - v1) * 100           # %/V
dropout_vin = next(vin for vin, vout, _ in rows if vout >= VOUT_NOM * 0.98)
at_5v = next(r for r in rows if abs(r[0] - 5.0) < 1e-9)
iq_est = at_5v[2] - at_5v[1] / RLOAD                   # IIN - IOUT at VIN = 5 V

print(f"line regulation : {line_reg:+.4f} %/V   ({v1:.2f} V → {v2:.2f} V)")
print(f"dropout (98 %)  : VIN = {dropout_vin:.2f} V  →  Vdo ≈ {(dropout_vin - VOUT_NOM * 0.98) * 1e3:.0f} mV")
print(f"VOUT @ 5.0 V    : {at_5v[1]:.6f} V")
print(f"Iq (estimate)   : {iq_est * 1e6:.1f} µA")
