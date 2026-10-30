"""NI Digital Pattern Editor project model for the TDC program.

Reads the pin map, specifications, levels, timing and compiled patterns (.digipat, TDMS) of a
project folder; decodes vectors (pin states + instruction); runs a pattern through an
interpreter to get a timeline and the TDC stimulus timing (START, HIT train, busy wait, loops);
and edits repeat / set_loop counts in place to make pattern variants without the compiler.

Compiled vector encoding (observed in Digital Pattern Editor 21.8 files, Architecture 1):
  PinState:    4 bits per pin, pin i in byte i // 2, even pins in the low nibble;
               values follow nidigital.PinState: 0, 1, L=3, H=4, X=5, M=6, V=7, D=8, E=9.
  Instruction: bits 31..28 opcode (0 next, 1 repeat, 4 set_loop, 5 end_loop, 7 jump_if, F halt),
               bits 27..23 timeset index, bits 22..16 flags, bits 15..0 operand
               (repeat / loop count, or label index; bit 15 of a jump_if operand is a flag).
"""

import bisect
import math
import os
import random
import re
import xml.etree.ElementTree as ET

import tdms_lite

PIN_STATES = {0: "0", 1: "1", 3: "L", 4: "H", 5: "X", 6: "M", 7: "V", 8: "D", 9: "E"}
OPS = {0: "", 1: "repeat", 4: "set_loop", 5: "end_loop", 7: "jump_if", 15: "halt"}
COUNT_MAX = 0xFFFF            # repeat / set_loop counts are 16-bit in this encoding
PREFIX = {"f": 1e-15, "p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6, "μ": 1e-6, "m": 1e-3, "": 1.0,
          "k": 1e3, "M": 1e6, "G": 1e9}
_QTY = re.compile(r"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*([fpnuµμmkMG]?)(s|V|A|Hz|Ω)?(?![A-Za-z_])")


def _local(tag):
    return tag.rsplit("}", 1)[-1]


def parse_quantity(text):
    """'1 µs' -> 1e-06, '15 mA' -> 0.015, '5' -> 5.0 (None when it is not a plain quantity)."""
    m = _QTY.fullmatch(text.strip())
    if not m:
        return None
    return float(m.group(1)) * PREFIX[m.group(2)]


def split_quantity(text):
    """'3.3 V' -> (3.3, 'V'), '10 mA' -> (10.0, 'mA'), '0' -> (0.0, '')."""
    m = _QTY.fullmatch(text.strip())
    if not m:
        return None, ""
    return float(m.group(1)), (m.group(2) or "") + (m.group(3) or "")


def evaluate(expr, symbols):
    """Evaluate a levels / specs formula such as '0.4*DC.VDD' or '15 mA' (SI base units)."""
    text = expr.strip()
    for name in sorted(symbols, key=len, reverse=True):
        text = text.replace(name, f"({symbols[name]!r})")
    text = _QTY.sub(lambda m: repr(float(m.group(1)) * PREFIX[m.group(2)]), text)
    if not re.fullmatch(r"[0-9eE.+\-*/() ]*", text) or not text.strip():
        raise ValueError(f"cannot evaluate {expr!r}")
    return float(eval(text, {"__builtins__": {}}, {}))  # noqa: S307 — digits and operators only


def fmt_time(seconds, digits=6):
    if seconds is None or not math.isfinite(seconds):
        return "—"
    if abs(seconds) >= 86400:
        return f"{seconds / 86400:.4g} 天"
    if abs(seconds) >= 3600:
        return f"{seconds / 3600:.4g} 小时"
    if abs(seconds) >= 60:
        return f"{seconds / 60:.4g} 分钟"
    for unit, scale in (("s", 1), ("ms", 1e-3), ("µs", 1e-6), ("ns", 1e-9)):
        if abs(seconds) >= scale * (1 - 1e-9) or unit == "ns":
            return f"{seconds / scale:.{digits}g} {unit}"
    return f"{seconds:g} s"


# --------------------------------------------------------------------------- project files

def load_pinmap(path):
    root = ET.parse(path).getroot()
    out = {"instruments": [], "pins": [], "groups": {}, "connections": {}}
    for el in root.iter():
        tag = _local(el.tag)
        if tag == "NIDigitalPatternInstrument":
            out["instruments"].append({"name": el.get("name"), "channels": int(el.get("numberOfChannels", 0))})
        elif tag == "DUTPin":
            out["pins"].append(el.get("name"))
        elif tag == "PinGroup":
            out["groups"][el.get("name")] = [r.get("pin") for r in el if _local(r.tag) == "PinReference"]
        elif tag == "Connection":
            out["connections"][el.get("pin")] = {"instrument": el.get("instrument"), "channel": int(el.get("channel")),
                                                 "site": int(el.get("siteNumber", 0))}
    return out


def load_specs(path):
    """[{section, symbol, definition, comment}] in file order."""
    root = ET.parse(path).getroot()
    out = []
    for section in root:
        if _local(section.tag) != "Section":
            continue
        for f in section:
            if _local(f.tag) != "Formula":
                continue
            item = {"section": section.get("name"), "symbol": f.get("symbol"), "definition": "", "comment": ""}
            for c in f:
                if _local(c.tag) == "Definition":
                    item["definition"] = (c.text or "").strip()
                elif _local(c.tag) == "Comment":
                    item["comment"] = (c.text or "").strip()
            out.append(item)
    return out


def spec_symbols(specs):
    """{'DC.VDD': 3.3, ...} evaluated in file order (later specs may use earlier ones)."""
    symbols = {}
    for s in specs:
        try:
            symbols[f"{s['section']}.{s['symbol']}"] = evaluate(s["definition"], symbols)
        except ValueError:
            pass
    return symbols


def write_specs(src, dst, values):
    """Copy a .specs file, replacing the definitions of {'DC.VDD': '3.0 V', ...}."""
    ET.register_namespace("", "http://www.ni.com/schemas/Semiconductor/Specifications.xsd")
    ET.register_namespace("f", "http://www.ni.com/schemas/Semiconductor/Formula.xsd")
    tree = ET.parse(src)
    for section in tree.getroot():
        for f in section:
            key = f"{section.get('name')}.{f.get('symbol')}"
            if key in values:
                for c in f:
                    if _local(c.tag) == "Definition":
                        c.text = values[key]
    tree.write(dst, encoding="utf-8", xml_declaration=True)


def load_levels(path):
    root = ET.parse(path).getroot()
    out = []
    for el in root.iter():
        if _local(el.tag) == "DigitalPinLevelSet":
            row = {"pin": el.get("pin")}
            for c in el:
                row[_local(c.tag)] = (c.text or "").strip()
            out.append(row)
    return out


def load_timing(path):
    root = ET.parse(path).getroot()
    out = {}
    for el in root.iter():
        if _local(el.tag) == "TimeSet":
            period = next((c.text for c in el if _local(c.tag) == "Period"), None)
            out[el.get("name")] = {"period_text": (period or "").strip(), "period": parse_quantity(period or "")}
    return out


# --------------------------------------------------------------------------- patterns

class Pattern:
    """A compiled .digipat; `counts` holds in-memory edits of repeat / set_loop operands."""

    def __init__(self, path):
        self.path = path
        self.file = os.path.splitext(os.path.basename(path))[0]
        self.data = open(path, "rb").read()
        self.tdms = tdms_lite.read(self.data)
        ch = lambda g, n: tdms_lite.channel(self.tdms, g, n)
        self.name = self.tdms["props"].get("name") or self.file          # burst start label
        self.pins = list(ch("PinItem", "PinName"))
        self.timesets = list(ch("TimeSet", "Name"))
        self.base_ins = [int(v) for v in ch("Pattern", "Instruction")]
        self.comments = list(ch("Pattern", "Comment"))
        self.label_names = list(ch("Pattern", "LocalLabelName"))
        self.label_locs = [int(v) for v in ch("Pattern", "LocalLabelLocation")]
        n = len(self.base_ins)
        self.comments += [""] * (n - len(self.comments))
        raw = list(ch("Pattern", "PinState"))
        width = (len(self.pins) + 1) // 2
        self.states = []
        for i in range(n):
            row = raw[i * width:(i + 1) * width]
            self.states.append("".join(PIN_STATES.get((row[k // 2] >> (4 * (k % 2))) & 0xF, "?")
                                       for k in range(len(self.pins))))
        self.edits = {}                 # vector index -> new operand
        self.mtime = os.path.getmtime(path)

    # ---- instructions ----
    @property
    def ins(self):
        return [self._apply_edit(i, v) for i, v in enumerate(self.base_ins)]

    def _apply_edit(self, i, v):
        return (v & ~0xFFFF) | self.edits[i] if i in self.edits else v

    @property
    def modified(self):
        return any(self._apply_edit(i, v) != v for i, v in enumerate(self.base_ins) if i in self.edits)

    def label_at(self, i):
        return next((n for n, loc in zip(self.label_names, self.label_locs) if loc == i), "")

    def section_of(self, i):
        """Nearest label at or above vector i."""
        best, where = "", -1
        for n, loc in zip(self.label_names, self.label_locs):
            if where < loc <= i:
                best, where = n, loc
        return best

    @staticmethod
    def decode(ins):
        op = (ins >> 28) & 0xF
        return {"op": op, "name": OPS.get(op, f"op{op:X}"), "timeset": (ins >> 23) & 0x1F,
                "flags": (ins >> 16) & 0x7F, "operand": ins & 0xFFFF}

    def instruction_text(self, i):
        d = self.decode(self.ins[i])
        op, arg = d["op"], d["operand"]
        label = lambda k: self.label_names[k] if 0 <= k < len(self.label_names) else f"#{k}"
        if op == 0:
            text = ""
        elif op in (1, 4):
            text = f"{d['name']}({arg})"
        elif op == 5:
            text = f"end_loop({label(arg)})"
        elif op == 7:
            text = f"jump_if(0x{d['flags']:02X}{'!' if arg & 0x8000 else ''}, {label(arg & 0x7FFF)})"
        elif op == 15:
            text = "halt"
        else:
            text = f"{d['name']}(0x{arg:04X})"
        if d["flags"] and op != 7:
            text = (text + f"  [flags 0x{d['flags']:02X}]").strip()
        return text

    def loop_end(self, i):
        """Index of the end_loop that closes the set_loop at vector i (nesting-aware)."""
        depth = 0
        for j in range(i + 1, len(self.base_ins)):
            op = (self.ins[j] >> 28) & 0xF
            if op == 4:
                depth += 1
            elif op == 5:
                if depth == 0:
                    return j
                depth -= 1
        return None

    def timeset_of(self, i):
        k = self.decode(self.ins[i])["timeset"]
        return self.timesets[k] if k < len(self.timesets) else f"ts{k}"

    def set_count(self, i, value):
        d = self.decode(self.base_ins[i])
        if d["op"] not in (1, 4):
            raise ValueError(f"vector {i} has no repeat / set_loop count")
        value = int(value)
        clamped = min(max(value, 1), COUNT_MAX)
        if clamped == d["operand"]:
            self.edits.pop(i, None)
        else:
            self.edits[i] = clamped
        return clamped, clamped != value

    def patched_bytes(self):
        data = self.data
        for i, v in sorted(self.edits.items()):
            data = tdms_lite.patch_u32(data, self.tdms, "Pattern", "Instruction", i, self._apply_edit(i, self.base_ins[i]))
        return data

    # ---- execution ----
    def run(self, periods, busy_delay=120e-6, rng=None, record_limit=20e-3, max_steps=400_000):
        """Interpret the pattern.

        The DUT model: the busy/ready pin (compared H by a jump_if wait loop) goes high
        `busy_delay` after the last drive edge, i.e. when the TDC has finished converting.
        Loops are extrapolated once the timeline of the first outer iteration is recorded.
        Returns a dict with the total time, timeline points and per-loop information.
        """
        ins = self.ins
        n = len(ins)
        loc = dict(enumerate(self.label_locs))
        dec = [self.decode(v) for v in ins]
        per, unknown = [], set()
        for d in dec:
            ts = self.timesets[d["timeset"]] if d["timeset"] < len(self.timesets) else None
            if periods.get(ts) is None:
                unknown.add(ts)
            per.append(periods.get(ts) or 1e-6)
        st = self.states
        # vector i drives a 0<->1 edge relative to vector j (cached per pair)
        edge_cache = {}

        def drive_edge(a, b):
            key = (a, b)
            if key not in edge_cache:
                edge_cache[key] = any(x in "01" and y in "01" and x != y for x, y in zip(st[a], st[b]))
            return edge_cache[key]

        program_cache = {}

        def program_loop(lo, hi):
            if (lo, hi) not in program_cache:
                program_cache[(lo, hi)] = any((ins[j] >> 28) in (4, 7) for j in range(lo, hi))
            return program_cache[(lo, hi)]
        t, pc, steps = 0.0, 0, 0
        stack = []
        recording = True
        timeline = []                      # (t, states) at each change
        notes = set()
        waits = []                         # (t_start, duration) of busy waits while recording
        last_drive_edge = 0.0
        prev_states = None
        prev_pc = None
        outer_iteration = None
        outer_loops = None
        outer_set_loop = None
        extrapolated = False
        delay = lambda: busy_delay * (rng.uniform(0.8, 1.2) if rng else 1.0)
        ready_at = delay()                 # busy pin high from here on; None = converting

        while pc < n:
            steps += 1
            if steps > max_steps:
                notes.add("步数超过上限，结果为估算")
                extrapolated = True
                break
            d = dec[pc]
            period = per[pc]
            states = st[pc]
            if prev_pc is not None and states is not prev_states and drive_edge(prev_pc, pc):
                last_drive_edge = t
            if recording and states != prev_states:
                timeline.append((t, states))
            prev_states, prev_pc = states, pc
            op, arg = d["op"], d["operand"]

            if op == 1:                                    # repeat
                t += period * max(1, arg)
                pc += 1
            elif op == 4:                                  # set_loop
                t += period
                stack.append({"count": max(1, arg), "t_iter": t, "first": True, "depth": len(stack) + 1, "pc": pc})
                pc += 1
            elif op == 5:                                  # end_loop
                t += period
                if not stack:
                    notes.add(f"v{pc}: end_loop 没有对应的 set_loop")
                    pc += 1
                    continue
                top = stack[-1]
                remaining = top["count"] - 1
                target = loc.get(arg & 0x7FFF, pc + 1)
                if target <= top["pc"]:
                    notes.add(f"v{pc}: end_loop({self.label_names[arg & 0x7FFF] if (arg & 0x7FFF) < len(self.label_names) else arg}) "
                              f"跳回到 v{top['pc']} 的 set_loop 之前，循环体可能写错了标签")
                if top["depth"] == 1 and top["first"] and program_loop(target, pc):
                    outer_iteration = t - top["t_iter"]
                    outer_loops = top["count"]
                    outer_set_loop = top["pc"]
                    if recording:
                        recording = False
                        timeline.append((t, states))
                top["first"] = False
                if remaining > 0 and not recording and remaining > 1:
                    t += (t - top["t_iter"]) * remaining
                    extrapolated = True
                    stack.pop()
                    pc += 1
                elif remaining > 0:
                    top["count"] = remaining
                    top["t_iter"] = t
                    pc = target
                else:
                    stack.pop()
                    pc += 1
            elif op == 7:                                  # jump_if: busy wait on a compared pin
                target = loc.get(arg & 0x7FFF)
                compares_high = "H" in states
                if target == pc and compares_high:
                    if ready_at is None:           # converting: ready busy_delay after the last stimulus edge
                        ready_at = last_drive_edge + delay()
                    cycles = max(1, math.ceil(max(0.0, ready_at - t) / period))
                    if recording:
                        waits.append((t, cycles * period))
                    t += cycles * period
                    ready_at = None
                    pc += 1
                else:
                    if target != pc:
                        notes.add(f"v{pc}: jump_if 到其他标签，仿真按条件不成立处理")
                    t += period
                    pc += 1
            elif op == 15:                                 # halt
                t += period
                break
            else:
                if op != 0:
                    notes.add(f"v{pc}: 未识别的指令 op{op:X}，按普通向量执行")
                t += period
                pc += 1
            if recording and (t > record_limit or len(timeline) > 20000):
                recording = False
                timeline.append((t, states))
        notes.update(f"未知时序 {ts}，按 1 µs 计" for ts in unknown)
        if recording or not timeline:              # close the trace; after extrapolation it is already closed
            timeline.append((t, prev_states or ""))
        return {"total": t, "timeline": timeline, "waits": waits, "outer_iteration": outer_iteration,
                "outer_loops": outer_loops, "outer_set_loop": outer_set_loop, "extrapolated": extrapolated, "notes": sorted(notes), "steps": steps}


def pulses(timeline, k):
    """Rising/falling edge times of pin k in a timeline [(t, states)]."""
    rises, falls, prev = [], [], None
    for t, states in timeline:
        s = states[k] if k < len(states) else None
        if prev == "0" and s == "1":
            rises.append(t)
        elif prev == "1" and s == "0":
            falls.append(t)
        if s in ("0", "1"):
            prev = s
    return rises, falls


def analyze(pattern, periods, busy_delay=120e-6):
    """TDC timing of a pattern: roles of the pins, START / HIT timing, loop and total time."""
    r = pattern.run(periods, busy_delay)
    tl = r["timeline"]
    roles, edges = {}, {}
    for k, pin in enumerate(pattern.pins):
        used = {s[k] for _, s in tl if k < len(s)}
        rises, falls = pulses(tl, k)
        edges[pin] = (rises, falls)
        if used & {"H", "L"}:
            roles[pin] = "BUSY"
    # START rises first (ties: fewer pulses); HIT is the busiest of the others. A lone pin that
    # pulses more than once is a HIT train.
    drive = sorted((p for p in pattern.pins if edges[p][0] and p not in roles),
                   key=lambda p: (edges[p][0][0], len(edges[p][0])))
    if len(drive) == 1 and len(edges[drive[0]][0]) > 1:
        roles[drive[0]] = "HIT"
    elif drive:
        roles[drive[0]] = "START"
        rest = sorted(drive[1:], key=lambda p: len(edges[p][0]))
        if rest:
            roles[rest[-1]] = "HIT"
        for p in rest[:-1]:
            roles[p] = "DRIVE"

    def fall_after(p, t):
        falls = edges[p][1]
        i = bisect.bisect_right(falls, t)
        return falls[i] - t if i < len(falls) else None

    def width(p):
        ws = [w for w in (fall_after(p, r0) for r0 in edges[p][0]) if w is not None]
        return sum(ws) / len(ws) if ws else None

    start = next((p for p, role in roles.items() if role == "START"), None)
    hit = next((p for p, role in roles.items() if role == "HIT"), None)
    busy = next((p for p, role in roles.items() if role == "BUSY"), None)
    hit_rises = edges[hit][0] if hit else []
    start_rise = edges[start][0][0] if start else None
    periods_hit = [b - a for a, b in zip(hit_rises, hit_rises[1:])]
    out = {
        "pattern": pattern.file, "label": pattern.name, "pins": pattern.pins, "roles": roles,
        "start_pin": start or "—", "hit_pin": hit or "—", "busy_pin": busy or "—",
        "start_width": width(start) if start else None,
        "hit_count": len(hit_rises),
        "hit_period": sum(periods_hit) / len(periods_hit) if periods_hit else None,
        "hit_width": width(hit) if hit else None,
        "start_to_hit": (hit_rises[0] - start_rise) if (hit_rises and start_rise is not None) else None,
        "busy_wait": sum(w for _, w in r["waits"]) if r["waits"] else None,
        "iteration": r["outer_iteration"] if r["outer_iteration"] is not None else r["total"],
        "loops": r["outer_loops"] or 1,
        "outer_set_loop": r["outer_set_loop"],
        "total": r["total"],
        "extrapolated": r["extrapolated"],
        "notes": r["notes"],
        "modified": pattern.modified,
    }
    events = []
    if start_rise is not None:
        events.append({"event": "START", "pin": start, "t": start_rise, "rel": 0.0, "width": out["start_width"]})
    t0 = start_rise if start_rise is not None else (hit_rises[0] if hit_rises else 0.0)
    for i, tr in enumerate(hit_rises):
        events.append({"event": f"HIT {i + 1}", "pin": hit, "t": tr, "rel": tr - t0,
                       "width": fall_after(hit, tr)})
    for t, w in r["waits"]:
        events.append({"event": "等待 BUSY", "pin": busy, "t": t, "rel": t - t0 if start_rise is not None else None, "width": w})
    events.sort(key=lambda e: e["t"])
    out["events"] = events
    used = {pattern.timeset_of(i) for i in range(len(pattern.base_ins))}
    out["timeline"] = timeline_capture(pattern, tl, roles, min((periods[u] for u in used if u in periods), default=None))
    return out


def timeline_capture(pattern, tl, roles, resolution=None):
    """Stacked digital traces in the scope widget format {t: [ms], channels: [{name, unit, data}]}."""
    ts, chans = [], [[] for _ in pattern.pins]
    level = {"0": 0.0, "1": 1.0, "L": 0.0, "H": 1.0, "X": 0.5, "M": 0.5, "V": 0.5, "D": 0.5, "E": 0.5}
    order = list(range(len(pattern.pins)))
    prev = None
    for t, states in tl:
        if prev is not None:
            ts.append(t * 1e3)
            for slot, k in enumerate(order):
                chans[k].append(level.get(prev[k], 0.5) * 0.8 + 1.2 * (len(order) - 1 - slot))
        ts.append(t * 1e3)
        for slot, k in enumerate(order):
            chans[k].append(level.get(states[k] if k < len(states) else "X", 0.5) * 0.8 + 1.2 * (len(order) - 1 - slot))
        prev = states
    names = [f"{p} ({roles[p]})" if p in roles else p for p in pattern.pins]
    return {"t": ts, "dt_ms": resolution * 1e3 if resolution else 0, "channels": [{"name": n, "unit": "", "data": c} for n, c in zip(names, chans)]}


class Project:
    def __init__(self, folder):
        self.folder = os.path.abspath(folder)
        find = lambda ext: sorted(os.path.join(dp, f) for dp, _, fs in os.walk(self.folder) for f in fs
                                  if f.lower().endswith(ext) and ".cache" not in dp)
        self.pinmap_path = (find(".pinmap") or [None])[0]
        self.specs_path = (find(".specs") or [None])[0]
        self.levels_path = (find(".digilevels") or [None])[0]
        self.timing_path = (find(".digitiming") or [None])[0]
        if not self.pinmap_path:
            raise FileNotFoundError(f"{folder}: 没有 .pinmap")
        self.pinmap = load_pinmap(self.pinmap_path)
        self.specs = load_specs(self.specs_path) if self.specs_path else []
        self.levels = load_levels(self.levels_path) if self.levels_path else []
        self.timing = load_timing(self.timing_path) if self.timing_path else {}
        self.patterns = {}
        self.pattern_dir = os.path.join(self.folder, "Patterns")
        self.rescan()

    def rescan(self):
        found = {}
        for dp, _, fs in os.walk(self.folder):
            for f in fs:
                if f.lower().endswith(".digipat") and ".cache" not in dp:
                    path = os.path.join(dp, f)
                    old = self.patterns.get(os.path.splitext(f)[0])
                    found[os.path.splitext(f)[0]] = old if old and old.mtime == os.path.getmtime(path) else Pattern(path)
        self.patterns = dict(sorted(found.items(), key=lambda kv: kv[0].lower()))

    @property
    def periods(self):
        return {k: v["period"] for k, v in self.timing.items() if v["period"]}

    def symbols(self):
        return spec_symbols(self.specs)
