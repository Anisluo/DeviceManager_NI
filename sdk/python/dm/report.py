"""Datasheet-style reports of stored runs (HTML with inline SVG, or PDF).

    import dm.report
    dm.report.export(42)                    # -> <data>/exports/run42_<name>.html
    dm.report.export(42, fmt="pdf")

What is plotted depends on the run:
  * shmoo  — pass/fail map over the two axes
  * scan   — every measured column vs the innermost axis, one curve per value of the outer axes
  * script — every numeric column vs the first column
plus a statistics table (count / min / max / mean / σ) and the run metadata.
"""

import datetime
import html
import io
import math
import os

from . import data

__all__ = ["export", "figures"]

PALETTE = ["#D97757", "#4F74A3", "#6E8F4E", "#B8892B", "#8D6A9F", "#3E8F86"]
INK, MUTED, GRID, PAPER = "#28261F", "#6E6A5E", "#E9E6DC", "#FBFAF7"
PASS, FAIL = "#6E9F6F", "#C9604F"


def _setup_matplotlib():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    installed = {f.name for f in font_manager.fontManager.ttflist}
    # Latin text in Segoe UI, Chinese falls back to a CJK font; only list fonts that exist.
    family = [f for f in ("Segoe UI", "Microsoft YaHei", "SimHei", "Noto Sans CJK SC", "PingFang SC")
              if f in installed] + ["DejaVu Sans"]
    plt.rcParams.update({
        "font.family": family,
        "font.size": 9,
        "axes.edgecolor": "#C9C4B6", "axes.labelcolor": INK, "axes.titlesize": 11,
        "axes.titleweight": "normal", "axes.titlecolor": INK, "axes.grid": True,
        "axes.prop_cycle": matplotlib.cycler(color=PALETTE),
        "axes.spines.top": False, "axes.spines.right": False,
        "grid.color": GRID, "grid.linewidth": 0.8,
        "xtick.color": MUTED, "ytick.color": MUTED,
        "legend.frameon": False, "figure.facecolor": "white",
        "lines.linewidth": 1.6, "lines.markersize": 3.5,
        "svg.fonttype": "path", "axes.unicode_minus": False,
    })
    return plt


def _numeric(values):
    return [v for v in values if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)]


def _stats(table):
    out = []
    for c in table.columns:
        vals = _numeric(table[c])
        if not vals or c == "pass":
            continue
        mean = sum(vals) / len(vals)
        std = math.sqrt(sum((v - mean) ** 2 for v in vals) / (len(vals) - 1)) if len(vals) > 1 else 0.0
        out.append((c, len(vals), min(vals), max(vals), mean, std))
    return out


def _eng(v):
    if v == 0 or not math.isfinite(v):
        return f"{v:g}"
    exp = int(math.floor(math.log10(abs(v)) / 3) * 3)
    exp = max(-15, min(9, exp))
    prefix = {-15: "f", -12: "p", -9: "n", -6: "µ", -3: "m", 0: "", 3: "k", 6: "M", 9: "G"}[exp]
    return f"{v / 10 ** exp:.5g}{prefix}"


def _units(table):
    units = {}
    for a in table.run.get("meta", {}).get("axes", []):
        units[a["name"]] = a.get("unit", "")
    return units


def figures(table):
    """List of (title, matplotlib Figure)."""
    plt = _setup_matplotlib()
    meta = table.run.get("meta", {})
    kind = table.run.get("kind")
    units = _units(table)
    figs = []

    def label(c):
        return f"{c} [{units[c]}]" if units.get(c) else c

    if kind == "shmoo" and meta.get("x") and meta.get("y"):
        import numpy as np
        x, y, z = meta["x"], meta["y"], meta.get("value", "pass")
        xs = sorted(set(table[x]))
        ys = sorted(set(table[y]))
        grid = np.full((len(ys), len(xs)), np.nan)
        for r in table.rows:
            grid[ys.index(r[y]), xs.index(r[x])] = 1.0 if r.get(z) in (True, 1) else 0.0
        fig, ax = plt.subplots(figsize=(7.2, 4.6))
        from matplotlib.colors import ListedColormap
        ax.pcolormesh(range(len(xs) + 1), range(len(ys) + 1), grid, cmap=ListedColormap([FAIL, PASS]),
                      vmin=0, vmax=1, edgecolors="white", linewidth=0.6)
        step_x = max(1, len(xs) // 10)
        step_y = max(1, len(ys) // 12)
        ax.set_xticks([i + 0.5 for i in range(0, len(xs), step_x)], [_eng(v) for v in xs[::step_x]])
        ax.set_yticks([i + 0.5 for i in range(0, len(ys), step_y)], [_eng(v) for v in ys[::step_y]])
        ax.set_xlabel(label(x))
        ax.set_ylabel(label(y))
        ax.grid(False)
        passed = sum(1 for r in table.rows if r.get(z) in (True, 1))
        ax.set_title(f"Shmoo · 通过 {passed}/{len(table.rows)}", loc="left")
        figs.append(("Shmoo", fig))
        return figs

    axes = [a["name"] for a in meta.get("axes", [])]
    if axes:
        inner, outer = axes[-1], axes[:-1]
    else:
        inner, outer = (table.columns[0] if table.columns else None), []
    if inner is None:
        return figs
    measured = [c for c in table.columns if c not in axes and c != inner and c != "pass" and _numeric(table[c])]
    groups = [()]
    if outer:
        groups = []
        for r in table.rows:
            key = tuple(r.get(o) for o in outer)
            if key not in groups:
                groups.append(key)
    for col in measured[:8]:
        fig, ax = plt.subplots(figsize=(7.2, 3.8))
        for key in groups[:12]:
            rows = [r for r in table.rows if tuple(r.get(o) for o in outer) == key]
            pts = sorted((r[inner], r[col]) for r in rows
                         if isinstance(r.get(inner), (int, float)) and isinstance(r.get(col), (int, float)))
            if not pts:
                continue
            name = ", ".join(f"{o}={_eng(v) if isinstance(v, float) else v}" for o, v in zip(outer, key)) or None
            ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o" if len(pts) <= 60 else None, label=name)
        ax.set_xlabel(label(inner))
        ax.set_ylabel(label(col))
        ax.set_title(f"{col} vs {inner}", loc="left")
        ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: _eng(v)))
        if outer:
            ax.legend(fontsize=8, loc="best")
        fig.tight_layout()
        figs.append((f"{col} vs {inner}", fig))
    return figs


def _svg(fig):
    buf = io.StringIO()
    fig.savefig(buf, format="svg", bbox_inches="tight")
    s = buf.getvalue()
    return s[s.find("<svg"):]


def _meta_rows(table):
    r = table.run
    rows = [("运行", f"#{r['id']}  {r['name']}"), ("类型", r.get("kind") or ""), ("工位", r.get("station") or ""),
            ("脚本", r.get("script") or ""), ("开始", r.get("started") or ""), ("结束", r.get("finished") or ""),
            ("状态", r.get("status") or ""), ("点数", str(len(table)))]
    for a in r.get("meta", {}).get("axes", []):
        rows.append((f"轴 {a['name']}", f"{_eng(a['min'])} … {_eng(a['max'])} {a.get('unit', '')} · {a['count']} 点"))
    if "pass" in table.columns:
        p = sum(1 for v in table["pass"] if v in (True, 1))
        rows.append(("通过率", f"{p}/{len(table)} ({100.0 * p / max(1, len(table)):.1f} %)"))
    return rows


def _html(table, figs):
    esc = html.escape
    meta = "".join(f"<tr><th>{esc(k)}</th><td>{esc(str(v))}</td></tr>" for k, v in _meta_rows(table))
    stats = "".join(
        f"<tr><td>{esc(c)}</td><td>{n}</td><td>{_eng(lo)}</td><td>{_eng(hi)}</td><td>{_eng(mean)}</td><td>{_eng(sd)}</td></tr>"
        for c, n, lo, hi, mean, sd in _stats(table))
    charts = "".join(f"<figure><figcaption>{esc(t)}</figcaption>{_svg(f)}</figure>" for t, f in figs)
    generated = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    return f"""<!doctype html>
<html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{esc(table.run['name'])} — 测试报告</title>
<style>
:root {{ --bg:#F5F4EE; --paper:{PAPER}; --ink:{INK}; --muted:{MUTED}; --rule:#E1DDD1; --accent:#D97757; }}
* {{ box-sizing:border-box }}
body {{ margin:0; background:var(--bg); color:var(--ink); font:14px/1.55 "Segoe UI","Microsoft YaHei",system-ui,sans-serif }}
main {{ max-width:960px; margin:0 auto; padding:48px 24px 64px }}
h1 {{ font:400 30px/1.2 Georgia,"Songti SC",serif; margin:0 0 6px }}
h2 {{ font:400 19px Georgia,serif; margin:36px 0 12px }}
.sub {{ color:var(--muted); margin:0 0 28px }}
.card {{ background:var(--paper); border:1px solid var(--rule); border-radius:14px; padding:20px 24px }}
table {{ border-collapse:collapse; width:100% }}
th, td {{ text-align:left; padding:6px 10px; border-bottom:1px solid var(--rule); font-variant-numeric:tabular-nums }}
.meta th {{ color:var(--muted); font-weight:400; width:120px }}
.stats th {{ color:var(--muted); font-weight:600; font-size:12px }}
figure {{ margin:0 0 20px; background:var(--paper); border:1px solid var(--rule); border-radius:14px; padding:16px }}
figcaption {{ color:var(--muted); font-size:12px; margin-bottom:8px }}
figure svg {{ width:100%; height:auto }}
footer {{ color:var(--muted); font-size:12px; margin-top:40px }}
@media print {{ body {{ background:white }} main {{ padding:0 }} figure, .card {{ break-inside:avoid }} }}
</style></head><body><main>
<h1>{esc(table.run['name'])}</h1>
<p class="sub">测试报告 · 运行 #{table.run['id']} · {esc(table.run.get('started') or '')}</p>
<div class="card"><table class="meta">{meta}</table></div>
<h2>统计</h2>
<div class="card"><table class="stats"><tr><th>列</th><th>点数</th><th>最小</th><th>最大</th><th>平均</th><th>σ</th></tr>{stats}</table></div>
<h2>曲线</h2>
{charts or '<p class="sub">没有可绘制的数值列。</p>'}
<footer>DeviceManager · 生成于 {generated}</footer>
</main></body></html>"""


def _pdf(table, figs, path):
    plt = _setup_matplotlib()
    from matplotlib.backends.backend_pdf import PdfPages
    with PdfPages(path) as pdf:
        fig = plt.figure(figsize=(8.27, 11.69))          # A4
        fig.text(0.08, 0.94, table.run["name"], fontsize=20, color=INK)
        fig.text(0.08, 0.915, f"测试报告 · 运行 #{table.run['id']}", fontsize=10, color=MUTED)
        rows = _meta_rows(table)
        ax = fig.add_axes([0.08, 0.60, 0.84, 0.29])
        ax.axis("off")
        t = ax.table(cellText=[[k, v] for k, v in rows], colWidths=[0.22, 0.78], loc="upper left", cellLoc="left")
        t.auto_set_font_size(False)
        t.set_fontsize(9)
        for (r, c), cell in t.get_celld().items():
            cell.set_edgecolor(GRID)
            cell.set_text_props(color=MUTED if c == 0 else INK)
        st = _stats(table)
        if st:
            ax2 = fig.add_axes([0.08, 0.08, 0.84, 0.46])
            ax2.axis("off")
            ax2.set_title("统计", loc="left", fontsize=12, color=INK)
            t2 = ax2.table(cellText=[[c, n, _eng(lo), _eng(hi), _eng(m), _eng(sd)] for c, n, lo, hi, m, sd in st],
                           colLabels=["列", "点数", "最小", "最大", "平均", "σ"], loc="upper left", cellLoc="left")
            t2.auto_set_font_size(False)
            t2.set_fontsize(9)
            for (r, c), cell in t2.get_celld().items():
                cell.set_edgecolor(GRID)
                if r == 0:
                    cell.set_text_props(color=MUTED, weight="bold")
        pdf.savefig(fig)
        plt.close(fig)
        for _, f in figs:
            pdf.savefig(f, bbox_inches="tight")
        info = pdf.infodict()
        info["Title"] = table.run["name"]
        info["Creator"] = "DeviceManager"


def export(run_id, fmt="html", path=None, db=None):
    """Write the report; returns the file path."""
    fmt = fmt.lower()
    if fmt not in ("html", "pdf"):
        raise ValueError("fmt must be html or pdf")
    table = data.load(run_id, db=db)
    figs = figures(table)
    try:
        path = path or data._export_path(table.run, fmt)
        if fmt == "html":
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(_html(table, figs))
        else:
            _pdf(table, figs, path)
    finally:
        import matplotlib.pyplot as plt
        for _, f in figs:
            plt.close(f)
    try:
        import _dmhost
        _dmhost.file_exported(path)
    except ImportError:
        pass
    return os.path.abspath(path)
