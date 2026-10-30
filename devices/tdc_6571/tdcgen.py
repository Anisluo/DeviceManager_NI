"""Four-pin TDC stimulus generator, written straight to a compiled .digipat (no pattern compiler).

  CLK   continuous clock: drive format return-to-low, state 1 in every vector, so it pulses once
        per cycle (also inside repeats) at the timeset period = 1 / clock frequency
  TRIG  input: wait for a low->high (or high->low) edge with the same compare + jump_if wait loop
        the DemoPrj-TDC patterns use
  OUT1  square wave, f1 for n1 cycles, starting d1 after the trigger
  OUT2  square wave, f2 for n2 cycles, starting d2 after the trigger
then idle for a while, and repeat the whole measurement `loops` times.

All edges are quantised to the clock period; the achieved frequencies and delays are reported.
The vectors are encoded like the editor's own output (see digiproj), using a template .digipat for
the file layout and property types.
"""

import os
import re
import xml.sax.saxutils as sx

import tdms_lite

STATE_CODE = {"0": 0, "1": 1, "L": 3, "H": 4, "X": 5}
MAX_REPEAT = 0xFFFF
MAX_VECTORS = 200_000
# instruction words copied from the DemoPrj-TDC wait loop (WaitForBusy): a compare vector with
# flags 0x20, then jump_if(flags 0x28, operand 0x8000 | label) on itself
PRE_WAIT_FLAGS = 0x20
WAIT_FLAGS = 0x28

DEFAULTS = {
    "name": "TDC_GEN", "clk_pin": "I3", "trig_pin": "I0", "out1_pin": "I1", "out2_pin": "I2",
    "clock_mhz": 50.0, "edge": "rising",
    "out1_khz": 250.0, "out1_cycles": 1, "out1_delay_us": 0.0,
    "out2_khz": 500.0, "out2_cycles": 16, "out2_delay_us": 200.0,
    "post_us": 1000.0, "loops": 1,
}


class GenError(ValueError):
    pass


def _ins(op, operand=0, flags=0, ts=0):
    return (op << 28) | (ts << 23) | (flags << 16) | (operand & 0xFFFF)


def plan(params, pins_available):
    """Validate the parameters and work out the quantised timing. Returns a dict."""
    p = dict(DEFAULTS)
    p.update({k: v for k, v in params.items() if v is not None and v != ""})
    name = str(p["name"]).strip()
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise GenError(f"图形名 {name!r} 无效：只能用字母、数字和下划线，且不能以数字开头")
    roles = {"CLK": p["clk_pin"], "TRIG": p["trig_pin"], "OUT1": p["out1_pin"], "OUT2": p["out2_pin"]}
    used = [v for v in roles.values() if v]
    for role, pin in roles.items():
        if pin and pin not in pins_available:
            raise GenError(f"{role} 引脚 {pin} 不在引脚映射里")
    if len(set(used)) != len(used):
        raise GenError("四个功能不能用同一个引脚")
    for k in ("out1_pin", "out2_pin", "trig_pin"):
        if not p[k]:
            raise GenError("TRIG、OUT1、OUT2 必须指定引脚")
    clock = float(p["clock_mhz"]) * 1e6
    if not 25e3 <= clock <= 200e6:
        raise GenError("时钟频率应在 0.025 … 200 MHz（6571 周期 5 ns … 40 µs）")
    T = 1.0 / clock
    # one timeset per clock period and CLK / TRIG pin choice, so sheets never disagree on a name
    timeset = f"tdcgen_{round(T * 1e12)}ps_{roles['CLK'] or 'noclk'}_{roles['TRIG']}"
    out = {"params": p, "name": name, "roles": roles, "period": T, "timeset": timeset, "warnings": []}
    for k in ("out1", "out2"):
        f = float(p[f"{k}_khz"]) * 1e3
        n = int(p[f"{k}_cycles"])
        if f <= 0 or n < 1:
            raise GenError(f"{k.upper()} 的频率和周期数必须大于 0")
        half = max(1, round(1.0 / (2 * f) / T))
        delay = max(0, round(float(p[f"{k}_delay_us"]) * 1e-6 / T))
        actual = 1.0 / (2 * half * T)
        if abs(actual - f) / f > 1e-6:
            out["warnings"].append(f"{k.upper()} 频率按时钟周期取整为 {actual / 1e3:.6g} kHz（要求 {f / 1e3:g} kHz）")
        out[k] = {"half": half, "delay": delay, "cycles": n, "freq": actual, "start": delay * T,
                  "end": (delay + 2 * half * n) * T}
    out["post"] = max(1, round(float(p["post_us"]) * 1e-6 / T))
    out["loops"] = int(p["loops"])
    if not 1 <= out["loops"] <= MAX_REPEAT:
        raise GenError(f"循环次数应在 1 … {MAX_REPEAT}")
    out["edge"] = "falling" if str(p["edge"]).startswith("f") else "rising"
    return out


def vectors(pl):
    """[(label, {pin: state}, instruction, comment)] for the plan."""
    roles = pl["roles"]
    clk, trig, o1, o2 = roles["CLK"], roles["TRIG"], roles["OUT1"], roles["OUT2"]
    idle = {trig: "X", o1: "0", o2: "0"}
    if clk:
        idle[clk] = "1"                                   # return-to-low: one pulse per cycle
    vec = []

    def add(label, states, ins, comment=""):
        vec.append((label, dict(idle, **states), ins, comment))

    def hold(states, cycles, comment=""):
        first = True
        while cycles > 0:
            n = min(cycles, MAX_REPEAT)
            add(None, states, _ins(1, n) if n > 1 else _ins(0), comment if first else "")
            first = False
            cycles -= n

    add(pl["name"], {}, _ins(0), "由 TDC测量 插件生成")
    loop = pl["loops"] > 1
    if loop:
        add(None, {}, _ins(4, pl["loops"]), f"重复 {pl['loops']} 次测量")
    first_level, second_level = ("L", "H") if pl["edge"] == "rising" else ("H", "L")
    add("Measure", {trig: first_level}, _ins(0, flags=PRE_WAIT_FLAGS), f"TRIG 先等{'低' if first_level == 'L' else '高'}电平")
    add("WaitFirst", {trig: first_level}, None, "")
    add(None, {trig: second_level}, _ins(0, flags=PRE_WAIT_FLAGS),
        "检测上升沿" if pl["edge"] == "rising" else "检测下降沿")
    add("WaitEdge", {trig: second_level}, None, "边沿到来后开始输出")

    # OUT1 / OUT2 edges in clock cycles after the trigger
    def edges(w):
        return [w["delay"] + k * w["half"] for k in range(2 * w["cycles"] + 1)]
    e1, e2 = edges(pl["out1"]), edges(pl["out2"])
    marks = sorted(set([0] + e1 + e2))
    end = marks[-1]

    def level(es, t):
        if t < es[0] or t >= es[-1]:
            return "0"
        return "1" if (t - es[0]) // (es[1] - es[0]) % 2 == 0 else "0"
    for a, b in zip(marks, marks[1:]):
        comment = ""
        if a == e1[0]:
            comment = f"OUT1 开始：{pl['out1']['freq'] / 1e3:.6g} kHz × {pl['out1']['cycles']}"
        if a == e2[0]:
            comment = (comment + "；" if comment else "") + f"OUT2 开始：{pl['out2']['freq'] / 1e3:.6g} kHz × {pl['out2']['cycles']}"
        hold({o1: level(e1, a), o2: level(e2, a)}, b - a, comment)
    if end == 0:
        hold({}, 1)
    hold({}, pl["post"], "输出结束后等待")
    if loop:
        add(None, {}, None, "")                          # end_loop(Measure), filled below
    add(None, {}, _ins(15, 0xF), "结束")
    if len(vec) > MAX_VECTORS:
        raise GenError(f"向量数 {len(vec)} 超过 {MAX_VECTORS}，请减少周期数或降低时钟频率")

    labels = [v[0] for v in vec if v[0]]
    index = {n: i for i, n in enumerate(labels)}
    out = []
    for i, (label, states, ins, comment) in enumerate(vec):
        if ins is None and label in ("WaitFirst", "WaitEdge"):
            ins = _ins(7, 0x8000 | index[label], flags=WAIT_FLAGS)
        elif ins is None:
            ins = _ins(5, index["Measure"])
        out.append((label, states, ins, comment))
    return out


def build(pl, template_bytes):
    """Bytes of the .digipat for a plan, using a compiled pattern as the layout template."""
    roles = pl["roles"]
    pins = [roles[r] for r in ("CLK", "TRIG", "OUT1", "OUT2") if roles[r]]
    vec = vectors(pl)
    tpl = tdms_lite.read(template_bytes)
    groups = tpl["groups"]

    def setc(group, chan, values):
        c = groups[group]["channels"][chan]
        c[0] = list(values)

    packed = []
    for _, states, _, _ in vec:
        row = [0] * ((len(pins) + 1) // 2)
        for k, pin in enumerate(pins):
            row[k // 2] |= STATE_CODE[states.get(pin, "X")] << (4 * (k % 2))
        packed += row
    labels = [(v[0], i) for i, v in enumerate(vec) if v[0]]
    referenced = {"Measure": pl["loops"] > 1, "WaitFirst": True, "WaitEdge": True}
    tpl["props"]["name"] = pl["name"]
    for k in ("description", "title", "author"):
        if k in tpl["props"]:
            tpl["props"][k] = ""
    setc("Header", "Comment", [])
    setc("TimeSet", "Name", [pl["timeset"]])
    setc("Import", "Name", [])
    setc("PinItem", "PinGroup", [])
    setc("PinItem", "PinGroupFormat", [])
    setc("PinItem", "PinName", pins)
    setc("PinItem", "PinNameGroupIndex", [-1] * len(pins))
    setc("Pattern", "Comment", [v[3] for v in vec])
    setc("Pattern", "LocalLabelName", [n for n, _ in labels])
    setc("Pattern", "LocalLabelLocation", [i for _, i in labels])
    setc("Pattern", "LocalLabelReferenced", [referenced.get(n, False) for n, _ in labels])
    setc("Pattern", "ExportLabelName", [])
    setc("Pattern", "ExportLabelLocation", [])
    setc("Pattern", "PinState", packed)
    setc("Pattern", "Instruction", [v[2] for v in vec])
    groups["Pattern"]["props"]["VectorCount"] = len(vec)
    return tdms_lite.write(tpl)


def _fmt_s(seconds):
    for unit, scale in (("µs", 1e-6), ("ns", 1e-9)):
        if seconds >= scale or unit == "ns":
            return f"{seconds / scale:.6g} {unit}"
    return f"{seconds:g} s"


def timing_sheet(pl, all_pins, folder):
    """Write <timeset>.digitiming (CLK return-to-low, TRIG strobed mid-cycle); returns its path."""
    path = os.path.join(folder, pl["timeset"] + ".digitiming")
    T = pl["period"]
    roles = pl["roles"]
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<TimingFile xmlns:xsd="http://www.w3.org/2001/XMLSchema" '
             'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" schemaVersion="1.0" '
             'xmlns="http://www.ni.com/Semiconductor/Timing">',
             '  <TimingSheet>', '    <TimeSets>', f'      <TimeSet name="{pl["timeset"]}">',
             f'        <Period>{_fmt_s(T)}</Period>', '        <PinEdges>']
    for pin in all_pins:
        lines.append(f'          <PinEdge pin="{sx.escape(pin)}">')
        if pin == roles["CLK"]:
            lines += ['            <DriveReturnLow>', '              <On>0</On>', '              <Data>0</Data>',
                      f'              <Return>{_fmt_s(T / 2)}</Return>', '              <Off>0</Off>',
                      '            </DriveReturnLow>']
        else:
            lines += ['            <DriveNonReturn>', '              <On>0</On>', '              <Data>0</Data>',
                      '              <Off>0</Off>', '            </DriveNonReturn>']
        strobe = _fmt_s(T / 2) if pin == roles["TRIG"] else "0"
        lines += ['            <CompareStrobe>', f'              <Strobe>{strobe}</Strobe>', '            </CompareStrobe>',
                  '            <DataSource>Pattern</DataSource>', '          </PinEdge>']
    lines += ['        </PinEdges>', '      </TimeSet>', '    </TimeSets>', '  </TimingSheet>', '</TimingFile>']
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    return path


def summary(pl):
    T = pl["period"]
    o1, o2 = pl["out1"], pl["out2"]
    measure = max(o1["end"], o2["end"]) + pl["post"] * T
    return {"clock_mhz": 1e-6 / T, "period": T, "timeset": pl["timeset"], "out1_khz": o1["freq"] / 1e3, "out2_khz": o2["freq"] / 1e3,
            "out1_start": o1["start"], "out2_start": o2["start"], "out1_to_out2": o2["start"] - o1["start"],
            "output_time": max(o1["end"], o2["end"]), "measure": measure, "loops": pl["loops"],
            "warnings": pl["warnings"]}


def sidecar_path(digipat_path):
    return os.path.splitext(digipat_path)[0] + ".tdcgen.json"


def quantised_note(pl):
    return "；".join(pl["warnings"]) or "无（频率、延时都是时钟周期的整数倍）"

