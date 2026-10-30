"""TDC test program on the PXIe-6571 (Digital Pattern Editor project DemoPrj-TDC).

The 6571 is the TDC's stimulus: it waits for the chip's BUSY/ready pin to rise (a jump_if wait
loop on an H compare), drives a START pulse, then a train of HIT pulses, waits, and repeats the
whole measurement N times. This plugin
  - reads the project (pin map, specs, levels, timing, compiled patterns) and decodes the
    vectors, so every pattern shows its START / HIT timing, loop count and run time;
  - edits repeat / set_loop counts in place to make variants (delays, hit count, pulse widths,
    loop count) without the pattern compiler, and saves them as new .digipat files;
  - bursts a pattern on the instrument without blocking (progress, abort, pass/fail), or in
    simulation runs the same program against a virtual TDC whose BUSY pin follows the stimulus.
"""

import copy
import os
import random
import time

from dm.plugin import Plugin, PluginError, method, E_STATE

# Helper modules are imported at load time, while the plugin folder is on sys.path.
import digiproj as dp
from tdc_hw import Hardware

HERE = os.path.dirname(os.path.abspath(__file__))
STATE_TEXT = {"idle": "空闲", "running": "运行中", "done": "完成", "aborted": "已中止", "error": "错误"}


class Tdc6571(Plugin):
    def __init__(self, config):
        super().__init__(config)
        o = self.options
        folder = o.get("project") or os.path.join(HERE, "project", "DemoPrj-TDC")
        if not os.path.isabs(folder):
            folder = os.path.join(HERE, folder)
        self.project = dp.Project(folder)
        self.work = o.get("work_dir") or os.path.join(HERE, "_work")
        os.makedirs(self.work, exist_ok=True)
        self.busy_delay = float(o.get("busy_delay_us", 120.0)) * 1e-6     # simulated TDC conversion time
        self.spec_values = {}                                               # "DC.VDD" -> "3.0 V"
        self._cache = {}
        names = list(self.project.patterns)
        if not names:
            raise PluginError(f"{folder}: 没有 .digipat 图形")
        self.current = o.get("pattern") if o.get("pattern") in self.project.patterns else names[0]
        self.run_state = {"state": "idle"}
        self.hw = None
        if not self.simulate:
            self.hw = Hardware(self.resource or self._instrument(), self.project,
                               driver_simulate=bool(o.get("driver_simulate", False)))
        self.log(f"TDC 工程 {os.path.basename(folder)}：{len(names)} 个图形，"
                 f"{'NI-Digital ' + (self.resource or self._instrument()) if self.hw else '仿真 TDC'}")

    def close(self):
        if self.hw:
            try:
                if self.run_state.get("state") == "running":
                    self.hw.abort()
            finally:
                self.hw.close()

    # ------------------------------------------------------------------ helpers
    def _instrument(self):
        inst = self.project.pinmap["instruments"]
        return inst[0]["name"] if inst else "PXI1Slot15"

    def _pattern(self, name=None):
        name = name or self.current
        p = self.project.patterns.get(name)
        if p is None:
            raise PluginError(f"没有图形 {name}")
        return p

    def _analysis(self, name=None):
        p = self._pattern(name)
        key = (p.file, p.mtime, tuple(sorted(p.edits.items())), self.busy_delay)
        if key not in self._cache:
            if len(self._cache) > 64:
                self._cache.clear()
            self._cache[key] = dp.analyze(p, self.project.periods, self.busy_delay)
        return self._cache[key]

    def _specs(self):
        specs = copy.deepcopy(self.project.specs)
        for s in specs:
            key = f"{s['section']}.{s['symbol']}"
            if key in self.spec_values:
                s["definition"] = self.spec_values[key]
        return specs

    def _run_file(self, p, single):
        """Path of the .digipat to burst: the original, or a patched copy in the work folder."""
        data = p.patched_bytes()
        if single:
            a = self._analysis(p.file)
            i = a.get("outer_set_loop")
            if i is not None:
                old = p.edits.get(i)
                p.edits[i] = 1
                data = p.patched_bytes()
                if old is None:
                    del p.edits[i]
                else:
                    p.edits[i] = old
        if data is p.data and not single:
            return p.path
        folder = os.path.join(self.work, "run")
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, p.file + ("_once" if single else "") + ".digipat")
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    # ------------------------------------------------------------------ project
    @method("Project, instrument and mode")
    def info(self):
        pr = self.project
        return {"project": pr.folder, "name": os.path.basename(pr.folder),
                "pinmap": os.path.basename(pr.pinmap_path or ""), "instrument": self._instrument(),
                "resource": self.resource or self._instrument(), "mode": "NI-Digital" if self.hw else "仿真",
                "patterns": len(pr.patterns), "current": self.current,
                "levels": os.path.basename(pr.levels_path or ""), "timing": os.path.basename(pr.timing_path or ""),
                "specs": os.path.basename(pr.specs_path or ""), "work_dir": self.work,
                "busy_delay_us": self.busy_delay * 1e6}

    @method("Pattern names for the selector")
    def list_patterns(self):
        self.project.rescan()
        return {"patterns": [{"label": n + (" *" if p.modified else ""), "value": n}
                             for n, p in self.project.patterns.items()]}

    @method("Select the pattern the other methods work on")
    def select(self, name: str):
        self._pattern(name)
        self.current = name
        return self.summary()

    @method("Re-read the project folder (new or changed .digipat files)")
    def rescan(self):
        self.project.rescan()
        return {"patterns": len(self.project.patterns)}

    # ------------------------------------------------------------------ analysis
    @method("TDC timing of the current pattern (START / HIT / BUSY, loops, run time)")
    def summary(self):
        a = self._analysis()
        out = {k: v for k, v in a.items() if k not in ("timeline", "events")}
        f = dp.fmt_time
        out.update({"total_text": f(a["total"]),
                    "iteration_text": f(a["iteration"]), "loops_text": f"× {a['loops']}",
                    "roles_text": "  ".join(f"{p}={r}" for p, r in a["roles"].items()) or "—",
                    "notes_text": "；".join(a["notes"]) or "无",
                    "current": self.current, "modified": a["modified"]})
        out["rows"] = [
            {"item": "START 引脚", "value": a["start_pin"]}, {"item": "HIT 引脚", "value": a["hit_pin"]},
            {"item": "BUSY 引脚", "value": a["busy_pin"]},
            {"item": "START 宽度", "value": f(a["start_width"])},
            {"item": "START → HIT1", "value": f(a["start_to_hit"])},
            {"item": "HIT 个数", "value": a["hit_count"]},
            {"item": "HIT 周期", "value": f(a["hit_period"])},
            {"item": "HIT 宽度", "value": f(a["hit_width"])},
            {"item": "BUSY 等待（仿真）", "value": f(a["busy_wait"])},
            {"item": "单次测量", "value": f(a["iteration"])},
            {"item": "循环次数", "value": a["loops"]},
            {"item": "总时间", "value": out["total_text"]},
        ]
        out["events_rows"] = [{"event": e["event"], "pin": e["pin"] or "—", "t": f(e["t"]),
                               "rel": f(e["rel"]) if e["rel"] is not None else "—",
                               "width": f(e["width"]) if e["width"] is not None else "—"}
                              for e in a["events"][:200]]
        return out

    @method("Timeline of the first measurement, stacked digital traces (scope format)")
    def timeline(self):
        return self._analysis()["timeline"]

    @method("Decoded vectors of the current pattern")
    def vectors(self):
        p = self._pattern()
        periods = self.project.periods
        rows = []
        for i in range(len(p.base_ins)):
            ts = p.timeset_of(i)
            rows.append({"v": i, "label": p.label_at(i), "states": p.states[i],
                         "instruction": p.instruction_text(i), "timeset": ts,
                         "period": dp.fmt_time(periods.get(ts)), "comment": p.comments[i],
                         "edited": "✎" if i in p.edits else ""})
        return {"pattern": p.file, "label": p.name, "pins": " ".join(p.pins), "rows": rows,
                "vector_count": len(rows)}

    # ------------------------------------------------------------------ edits
    @method("Editable repeat / set_loop counts of the current pattern (params widget rows)")
    def counts(self):
        p = self._pattern()
        periods = self.project.periods
        rows = []
        for i, ins in enumerate(p.ins):
            d = p.decode(ins)
            if d["op"] not in (1, 4):
                continue
            ts = p.timeset_of(i)
            period = periods.get(ts) or 0
            if d["op"] == 1:
                unit, note = f"× {dp.fmt_time(period)}", f"= {dp.fmt_time(d['operand'] * period)}"
            else:
                end = p.loop_end(i)
                body = p.label_names[p.decode(p.ins[end])["operand"] & 0x7FFF] if end is not None else "?"
                unit, note = "次", f"循环 {body}…v{end:02d}" if end is not None else "循环（没有找到 end_loop）"
            comment = p.comments[i]
            rows.append({"name": f"v{i:02d} {d['name']}", "group": p.section_of(i) or p.name,
                         "value": d["operand"], "unit": unit, "min": 1, "max": dp.COUNT_MAX, "type": "int",
                         "readonly": False, "note": (note + ("  · " + comment if comment else "")
                                                     + ("  · 已修改" if i in p.edits else ""))})
        return {"rows": rows, "modified": p.modified}

    @method("Change a repeat / set_loop count (name 'v07 repeat' or vector index)")
    def set_count(self, name: str, value: float):
        p = self._pattern()
        try:
            i = int(str(name).split()[0].lstrip("v"))
        except ValueError:
            raise PluginError(f"无效的向量 {name}")
        if not 0 <= i < len(p.base_ins):
            raise PluginError(f"向量 {i} 超出范围")
        try:
            applied, clamped = p.set_count(i, value)
        except ValueError as e:
            raise PluginError(str(e))
        return {"name": name, "value": applied, "clamped": clamped}

    @method("Discard the count edits of the current pattern")
    def revert(self):
        self._pattern().edits.clear()
        return self.summary()

    @method("Save the edited pattern as <name>.digipat next to the original and select it")
    def save_as(self, name: str = ""):
        p = self._pattern()
        name = (name or "").strip() or f"{p.file}_v{time.strftime('%m%d_%H%M%S')}"
        if any(c in name for c in '\\/:*?"<>|'):
            raise PluginError("文件名不能包含 \\ / : * ? \" < > |")
        path = os.path.join(os.path.dirname(p.path), name + ".digipat")
        if os.path.exists(path):
            raise PluginError(f"{name}.digipat 已存在")
        with open(path, "wb") as fh:
            fh.write(p.patched_bytes())
        p.edits.clear()
        self.project.rescan()
        self.current = name
        self.log(f"已保存 {name}.digipat（起始标签仍为 {p.name}）")
        return {"saved": path, "current": name}

    # ------------------------------------------------------------------ burst
    @method("Burst the current pattern (single=true: outer loop count 1); returns immediately")
    def burst(self, single: bool = False):
        if self.run_state.get("state") == "running":
            raise PluginError("图形正在运行", E_STATE)
        p = self._pattern()
        a = self._analysis()
        est = a["iteration"] if single else a["total"]
        st = {"state": "running", "pattern": p.file, "label": p.name, "single": bool(single),
              "t0": time.monotonic(), "est": est, "iteration": a["iteration"],
              "loops": 1 if single else a["loops"], "pass": None, "fails": {}}
        if self.hw:
            path = self._run_file(p, single)
            try:
                self.hw.start(path, p.name)
            except Exception as e:  # noqa: BLE001
                self.run_state = {"state": "error", "pattern": p.file, "error": str(e)}
                raise PluginError(f"burst 失败：{e}")
        else:
            # The simulated run takes the program's real duration, with a jittered BUSY time.
            r = p.run(self.project.periods, self.busy_delay, rng=random.Random())
            st["est"] = r["outer_iteration"] if single and r["outer_iteration"] else r["total"]
        self.run_state = st
        self.log(f"burst {p.file}（标签 {p.name}{'，单次' if single else ''}），预计 {dp.fmt_time(st['est'])}")
        return self.status()

    @method("Abort a running burst")
    def abort(self):
        st = self.run_state
        if st.get("state") != "running":
            return self.status()
        if self.hw:
            self.hw.abort()
        st["state"] = "aborted"
        st["t_end"] = time.monotonic()
        self.log(f"已中止 {st['pattern']}", "warning")
        return self.status()

    @method("Burst state, progress and result")
    def status(self):
        st = self.run_state
        if st.get("state") == "running":
            elapsed = time.monotonic() - st["t0"]
            finished = self.hw.done() if self.hw else elapsed >= st["est"]
            if finished:
                st["state"] = "done"
                # a simulated run ends exactly when the program would, not at the next poll
                st["t_end"] = time.monotonic() if self.hw else st["t0"] + st["est"]
                if self.hw:
                    st["pass"], st["fails"] = self.hw.result(self._pattern(st["pattern"]).pins
                                                             if st["pattern"] in self.project.patterns else [])
                else:
                    st["pass"], st["fails"] = True, {}
                self.log(f"{st['pattern']} 完成：{'通过' if st['pass'] else '失败' if st['pass'] is False else '结束'}，"
                         f"用时 {dp.fmt_time(st['t_end'] - st['t0'], 4)}", "info" if st["pass"] is not False else "error")
        state = st.get("state", "idle")
        out = {"state": state, "state_text": STATE_TEXT.get(state, state), "pattern": st.get("pattern", self.current),
               "running": state == "running", "mode": "NI-Digital" if self.hw else "仿真"}
        if "t0" in st:
            elapsed = (st.get("t_end") or time.monotonic()) - st["t0"]
            est = max(st["est"], 1e-9)
            out.update({"elapsed": elapsed, "elapsed_text": dp.fmt_time(elapsed), "est_text": dp.fmt_time(st["est"]),
                        "progress": 100.0 if state == "done" else min(99.9, 100.0 * elapsed / est),
                        "loops_done": st["loops"] if state == "done" else
                                      min(st["loops"], int(elapsed / st["iteration"])) if st.get("iteration") else 0,
                        "loops": st["loops"]})
            out["time_text"] = f"{dp.fmt_time(elapsed, 3)} / {dp.fmt_time(st['est'], 3)}"
            out["loops_text"] = f"{out['loops_done']} / {st['loops']}"
        else:
            out["time_text"] = out["loops_text"] = "—"
        if state in ("done", "aborted", "error"):
            out["pass"] = st.get("pass")
            out["result"] = ("PASS" if st.get("pass") else "FAIL" if st.get("pass") is False
                             else "ABORT" if state == "aborted" else "ERROR" if state == "error" else "—")
            fails = {k: v for k, v in st.get("fails", {}).items() if v}
            out["fail_text"] = "，".join(f"{k}: {v}" for k, v in fails.items()) or "无"
            if st.get("error"):
                out["error"] = st["error"]
        else:
            out["result"] = "—"
        return out

    # ------------------------------------------------------------------ project sheets
    @method("Specifications as editable rows (params widget)")
    def specs(self):
        rows = []
        symbols = dp.spec_symbols(self._specs())
        for s in self._specs():
            key = f"{s['section']}.{s['symbol']}"
            value, unit = dp.split_quantity(s["definition"])
            rows.append({"name": key, "group": s["section"], "value": value if value is not None else symbols.get(key, 0),
                         "unit": unit, "min": 0, "max": 0, "type": "float", "readonly": value is None,
                         "note": (s["comment"] + ("  · 已修改" if key in self.spec_values else "")).strip()})
        return {"rows": rows}

    @method("Change a specification value (keeps its unit), re-apply levels and timing")
    def set_spec(self, name: str, value: float):
        spec = next((s for s in self.project.specs if f"{s['section']}.{s['symbol']}" == name), None)
        if spec is None:
            raise PluginError(f"没有规格 {name}")
        _, unit = dp.split_quantity(self.spec_values.get(name, spec["definition"]))
        text = f"{value:g} {unit}".strip()
        if dp.split_quantity(spec["definition"])[0] is not None and text == spec["definition"]:
            self.spec_values.pop(name, None)
        else:
            self.spec_values[name] = text
        if self.hw:
            path = None
            if self.spec_values:
                path = os.path.join(self.work, "specs", os.path.basename(self.project.specs_path))
                os.makedirs(os.path.dirname(path), exist_ok=True)
                dp.write_specs(self.project.specs_path, path, self.spec_values)
            self.hw.apply(path or self.project.specs_path)
        self.log(f"规格 {name} = {text}")
        return {"name": name, "value": float(value), "clamped": False}

    @method("Pin levels with formulas evaluated against the specifications")
    def levels(self):
        symbols = dp.spec_symbols(self._specs())
        rows = []
        for lv in self.project.levels:
            row = {"pin": lv["pin"], "mode": lv.get("TerminationMode", "")}
            formulas = []
            for k in ("Vil", "Vih", "Vol", "Voh", "Iol", "Ioh", "Vterm"):
                text = lv.get(k, "")
                try:
                    v = dp.evaluate(text, symbols)
                    row[k.lower()] = f"{v * 1e3:g} mA" if k.startswith("I") else f"{v:g} V"
                    if any(name in text for name in symbols):
                        formulas.append(f"{k} = {text}")
                except ValueError:
                    row[k.lower()] = text
            row["formulas"] = "，".join(formulas) or "—"
            rows.append(row)
        return {"rows": rows, "file": os.path.basename(self.project.levels_path or "")}

    @method("Timesets of the timing sheet")
    def timesets(self):
        used = {}
        for name, p in self.project.patterns.items():
            for ts in p.timesets:
                used.setdefault(ts, []).append(name)
        return {"rows": [{"name": n, "period": t["period_text"], "used_by": len(used.get(n, [])),
                          "patterns": ", ".join(used.get(n, [])[:2]) + (f" 等 {len(used[n])} 个" if len(used.get(n, [])) > 2 else "")}
                         for n, t in self.project.timing.items()]}

    @method("Pin map: pins, channels, groups")
    def pins(self):
        pm = self.project.pinmap
        groups = {p: [g for g, ps in pm["groups"].items() if p in ps] for p in pm["pins"]}
        used = {}
        for name, p in self.project.patterns.items():
            for pin in p.pins:
                used[pin] = used.get(pin, 0) + 1
        return {"rows": [{"pin": p, "instrument": pm["connections"].get(p, {}).get("instrument", "—"),
                          "channel": pm["connections"].get(p, {}).get("channel", "—"),
                          "groups": ", ".join(groups[p]), "patterns": used.get(p, 0)} for p in pm["pins"]]}

    @method("[sim] TDC conversion time before BUSY rises again")
    def sim_set_busy(self, us: float):
        self.busy_delay = max(0.0, float(us)) * 1e-6
        return {"busy_delay_us": self.busy_delay * 1e6}
