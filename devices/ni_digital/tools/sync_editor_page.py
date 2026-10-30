"""Regenerate the project editor tabs of ni_digital/page.json from tdc_measure/page.json.

The editor is the same code in both places, so its page is too: this copies the tabs, prefixing
methods with "pe." (see pattern_core.py) and source / form ids with "pe_" so they cannot clash
with the driver's own. Run after changing those tabs:

    python devices/ni_digital/tools/sync_editor_page.py
"""

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "page.json")
TASK_PAGE = os.path.join(HERE, "..", "..", "tdc_measure", "page.json")
PREFIX = "pe_"
METHOD_KEYS = ("method", "write", "write_many", "undo", "delete_method", "select", "add", "remove")
SOURCES = ["sum", "info", "vec", "lv", "ts", "pins", "edges", "pedges", "inst"]
# the driver's own tabs and sources that stay; the methods behind the others stay for scripts
KEEP_TABS = ["PPMU"]
KEEP_SOURCES = ["pm"]


def text(id_, label, placeholder=""):
    return {"type": "text", "id": id_, "label": label, "value": "", "placeholder": placeholder, "mono": True}


def button(label, method, args=None, style="secondary", **extra):
    return dict({"type": "button", "label": label, "style": style,
                 "action": {"method": method, "args": args or {}}}, **extra)


WAVE_SECTION = {
    "title": "仿真波形",
    "subtitle": "点“仿真”才计算和刷新。每个引脚一条通道，上下限取自电平。滚轮或左键框选缩放时间，右键拖动平移，双击回到全部时间；"
                "拖动光标 C1、C2 测量时间，靠近边沿自动吸附，也可在顶部标尺左键放 C1、右键放 C2",
    "span": 12,
    "widgets": [
        button("仿真", "timeline", style="primary", publish="tl"),
        {"type": "logic", "source": "tl", "hint": "点“仿真”生成波形",
         "stale_source": "sum", "stale_field": "wave_rev"},
    ],
}

def spreadsheet(sec, delete, add, editors):
    """Make a section one table and one add button: cells are edited in place, Delete removes rows."""
    table = next(w for w in sec["widgets"] if w["type"] == "edit_table")
    table["delete_method"] = delete
    table["follow_new"] = True
    for action in table.get("row_actions", []):
        action["menu_only"] = True
    for col in table["columns"]:
        col.update(editors.get(col["field"], {}))
    sec["widgets"] = [table] + ([add] if add else [])


SPEC_SECTION = {
    "title": "规格（Specifications）",
    "subtitle": "直接在表里改；定义可写数值、单位或公式，电平和时序里用 DC.VDD 这样的名字引用，改名时公式一起改。"
                "选中行按 Delete 删除",
    "span": 12,
    "widgets": [
        {
            "type": "edit_table",
            "source": "spec_rows",
            "rows_field": "rows",
            "key": ["key"],
            "write": "set_spec_text",
            "delete_method": "remove_spec",
            "follow_new": True,
            "height": 520,
            "also": ["lv", "edges", "ts", "sum", "tl", "chk"],
            "columns": [
                {"field": "section", "label": "分组", "width": 120, "editor": "text"},
                {"field": "symbol", "label": "符号", "width": 160, "editor": "text", "mono": True},
                {"field": "definition", "label": "定义", "width": 240, "editor": "text"},
                {"field": "value", "label": "求值", "width": 140},
                {"field": "comment", "label": "注释", "editor": "text"},
            ],
            "row_actions": [{"label": "删除行", "method": "remove_spec", "menu_only": True}],
        },
        button("添加规格", "add_spec", refresh=["spec_rows"]),
    ],
}


def rewrite(node):
    """Prefixed copy of a page fragment."""
    if isinstance(node, list):
        return [rewrite(v) for v in node]
    if not isinstance(node, dict):
        return node.replace("$form.", "$form." + PREFIX) if isinstance(node, str) else node
    out = {}
    for key, value in node.items():
        if key == "args":                       # names here belong to the method, only $form values change
            out[key] = {k: rewrite(v) for k, v in value.items()}
        elif key in METHOD_KEYS and isinstance(value, str):
            out[key] = "pe." + value
        elif key in ("id", "source", "publish", "stale_source") and isinstance(value, str):
            out[key] = PREFIX + value
        elif key in ("also", "refresh"):
            out[key] = [PREFIX + v for v in value if v != "chk"]
        else:
            out[key] = rewrite(value)
    return out


def main():
    page = json.load(open(PAGE, encoding="utf-8"))
    task = json.load(open(TASK_PAGE, encoding="utf-8"))
    tabs = {t["title"]: t for t in task["tabs"]}

    page["sources"] = [s for s in page["sources"] if s["id"] in KEEP_SOURCES]
    page["sources"] += [rewrite(s) for s in task["sources"] if s["id"] in SOURCES]
    # every source keeps polling while the driver's own tabs are open, so keep them slow
    for s in page["sources"]:
        if s["id"].startswith(PREFIX):
            s["interval_ms"] = max(s.get("interval_ms", 2000), 1000)

    page["tabs"] = [t for t in page["tabs"] if t["title"] in KEEP_TABS]

    def section(tab, title):
        return json.loads(json.dumps(next(s for s in tabs[tab]["sections"] if s["title"] == title)))

    pin_map = section("引脚 / 工程", "引脚映射")
    pin_map["span"] = 12
    instruments = section("引脚 / 工程", "仪器")
    instruments["span"] = 12
    levels = section("电平 / 时序", "引脚电平")
    timesets = section("电平 / 时序", "时序集")
    timesets["span"] = 12
    edges = section("电平 / 时序", "时序集引脚边沿")
    spreadsheet(pin_map, "remove_pin", button("添加引脚", "add_pin", refresh=["pins", "chk"]),
                {"pin": {"editor": "text", "mono": True}})
    pin_map["subtitle"] = "直接在表里改，引脚改名时电平、时序和图形一起改；分组用逗号分隔，写新名字就新建分组。选中行按 Delete 删除"
    pin_map["widgets"][0]["also"] = ["chk", "vec", "lv", "edges", "pedges"]
    spreadsheet(levels, "remove_level", button("添加电平", "add_level", refresh=["lv", "chk"]),
                {"pin": {"editor": "combo", "options_field": "pin_list"}})
    levels["subtitle"] = "直接在表里改，可写数值或规格公式（如 0.6*DC.VDD），下次运行时重新加载。选中行按 Delete 删除"
    spreadsheet(timesets, "remove_timeset", button("添加时序集", "add_timeset", refresh=["ts", "edges"]),
                {"name": {"editor": "text", "mono": True}})
    timesets["subtitle"] = "直接在表里改；周期可写数值或规格公式，时序集改名时图形一起改。“添加时序集”复制最后一个。选中行按 Delete 删除"
    timesets["widgets"][0]["also"] = list(dict.fromkeys(timesets["widgets"][0].get("also", []) + ["vec"]))
    spreadsheet(edges, "remove_edge", None, {"pin": {"editor": "combo", "options_field": "pin_list"}})
    edges["widgets"][0]["add"] = {"label": "添加引脚条目", "method": "add_edge", "keys": ["timeset"]}
    edges["subtitle"] = "每个引脚 / 引脚组的驱动格式和边沿，可写数值或规格公式；引脚自己的条目优先于引脚组；NR 没有 Return。" \
                        "“添加引脚条目”加到选中行所在的时序集，没有选中时加到最后一个。选中行按 Delete 删除"

    table = json.loads(json.dumps(section("向量", "向量表")))
    table["title"] = "向量"
    table["span"] = 9
    table["subtitle"] = table["subtitle"].rstrip("。") + "。修改直接写入文件"
    table["widgets"][0]["also"] = list(dict.fromkeys(table["widgets"][0].get("also", []) + ["tree"]))
    tree = {"title": "向量表", "subtitle": "选中后 Delete 删除", "span": 3, "widgets": [
        {"type": "tree", "source": "tree", "select": "select", "add": "pattern_new", "remove": "pattern_delete",
         "noun": "向量表", "height": 620, "also": ["vec", "sum", "chk", "pedges"]}]}
    page["sources"].append({"id": PREFIX + "tree", "method": "pe.pattern_tree", "interval_ms": 2000})
    pattern = [tree, table, WAVE_SECTION, section("向量", "本图形用到的时序边沿")]
    page["sources"].append({"id": PREFIX + "spec_rows", "method": "pe.spec_rows", "interval_ms": 2000})

    for tab in ({"title": "引脚映射", "sections": [pin_map, instruments]},
                {"title": "规格", "sections": [SPEC_SECTION]},
                {"title": "电平", "sections": [levels]},
                {"title": "时序", "sections": [timesets, edges, WAVE_SECTION]},
                {"title": "向量 Vector", "sections": pattern}):
        # on this page a compiled pattern is called a vector table, as in the sheets it sits next to
        text_ = json.dumps(tab, ensure_ascii=False).replace("图形", "向量表")
        page["tabs"].append(rewrite(json.loads(text_)))

    for tab in page["tabs"]:                    # no full-width buttons on this page
        for sec in tab["sections"]:
            for widget in sec.get("widgets", []):
                if widget.get("type") == "button":
                    widget["fit"] = True

    with open(PAGE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(page, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
