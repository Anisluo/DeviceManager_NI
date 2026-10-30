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
RUN_SECTIONS = ["运行状态", "第一次测量的时序图", "检查"]
SOURCES = ["st", "sum", "tl", "info", "cnt", "vec", "specs", "lv", "ts", "pins", "edges", "pedges", "chk", "inst"]


def text(id_, label, placeholder=""):
    return {"type": "text", "id": id_, "label": label, "value": "", "placeholder": placeholder, "mono": True}


def button(label, method, args=None, style="secondary", **extra):
    return dict({"type": "button", "label": label, "style": style,
                 "action": {"method": method, "args": args or {}}}, **extra)


ALL = ["spec_rows", "vec", "sum", "tl", "cnt", "chk", "pedges", "edges", "ts", "lv", "pins", "inst", "info", "specs"]

PROJECT_TAB = {
    "title": "工程",
    "sections": [
        {
            "title": "工程",
            "subtitle": "新建的工程保存在驱动的 _work/projects 里；也可以打开任意已有工程的完整路径",
            "span": 4,
            "widgets": [
                {"type": "combo", "id": "project", "label": "工程",
                 "options_from": {"method": "project_list", "field": "projects"},
                 "init": {"method": "info", "field": "project"},
                 "action": {"method": "project_open", "args": {"path": "$value"}}},
                {"type": "readout", "label": "文件夹", "source": "info", "field": "project", "digits": 5},
                text("new_project", "新工程", "名称"),
                button("新建工程", "project_new", {"name": "$form.new_project"}, reload_page=True, refresh=ALL),
                text("open_path", "打开", "工程文件夹的完整路径"),
                button("打开工程", "project_open", {"path": "$form.open_path"}, reload_page=True, refresh=ALL),
            ],
        },
        {
            "title": "图形",
            "subtitle": "新图形只有一个 halt 向量，用到所有已接通道的引脚；到“向量”标签编辑",
            "span": 4,
            "widgets": [
                {"type": "combo", "id": "pattern", "label": "图形",
                 "options_from": {"method": "list_patterns", "field": "patterns"},
                 "init": {"method": "info", "field": "current"},
                 "action": {"method": "select", "args": {"name": "$value"}}},
                text("new_pattern", "新图形", "名称"),
                button("新建图形", "pattern_new", {"name": "$form.new_pattern"}, reload_page=True, refresh=ALL),
                button("运行", "burst", {"single": False}, style="primary"),
                button("单次试跑", "burst", {"single": True}),
                button("中止", "abort", style="ghost"),
            ],
        },
        {
            "title": "导出为插件",
            "subtitle": "生成 devices/<id>：运行器 DLL、页面和工程副本；点右上角“重新扫描插件”后出现在“插件”里",
            "span": 4,
            "widgets": [
                text("export_id", "插件 id", "字母、数字、下划线"),
                text("export_name", "显示名称", "留空用 id"),
                button("导出插件", "export_plugin", {"id": "$form.export_id", "name": "$form.export_name"},
                       style="primary", publish="export"),
                {"type": "readout", "label": "导出到", "source": "export", "field": "text", "digits": 5},
            ],
        },
    ],
}


SPEC_SECTION = {
    "title": "规格（Specifications）",
    "subtitle": "分组.符号，定义可写数值、单位或公式；电平和时序里用 DC.VDD 这样的名字引用",
    "span": 12,
    "widgets": [
        {
            "type": "edit_table",
            "source": "spec_rows",
            "rows_field": "rows",
            "key": ["key"],
            "write": "set_spec_text",
            "height": 520,
            "also": ["lv", "edges", "ts", "sum", "tl", "chk"],
            "columns": [
                {"field": "section", "label": "分组", "width": 120},
                {"field": "symbol", "label": "符号", "width": 160, "mono": True},
                {"field": "definition", "label": "定义", "width": 240, "editor": "text"},
                {"field": "value", "label": "求值", "width": 140},
                {"field": "comment", "label": "注释", "editor": "text"},
            ],
            "row_actions": [{"label": "删除行", "method": "remove_spec"}],
        },
        text("new_spec", "新规格", "分组.符号，如 DC.VCC"),
        text("new_spec_def", "定义", "如 3.3 V"),
        button("添加规格", "add_spec", {"key": "$form.new_spec", "definition": "$form.new_spec_def"}, refresh=["spec_rows"]),
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
        elif key in ("method", "write", "write_many", "undo") and isinstance(value, str):
            out[key] = "pe." + value
        elif key in ("id", "source", "publish") and isinstance(value, str):
            out[key] = PREFIX + value
        elif key in ("also", "refresh"):
            out[key] = [PREFIX + v for v in value]
        else:
            out[key] = rewrite(value)
    return out


def main():
    page = json.load(open(PAGE, encoding="utf-8"))
    task = json.load(open(TASK_PAGE, encoding="utf-8"))
    tabs = {t["title"]: t for t in task["tabs"]}

    page["sources"] = [s for s in page["sources"] if not s["id"].startswith(PREFIX)]
    page["sources"] += [rewrite(s) for s in task["sources"] if s["id"] in SOURCES]
    # every source keeps polling while the driver's own tabs are open, so keep them slow
    for s in page["sources"]:
        if s["id"].startswith(PREFIX):
            s["interval_ms"] = max(s.get("interval_ms", 2000), 1000)

    # the documents of a Digital Pattern Editor project, in its order
    titles = {"工程", "引脚映射", "规格", "电平", "时序", "图形编辑"}
    page["tabs"] = [t for t in page["tabs"] if t["title"] not in titles and t["title"] not in ("向量", "电平 / 时序")]
    project = json.loads(json.dumps(PROJECT_TAB))
    project["sections"] += [s for s in tabs["运行"]["sections"] if s["title"] in RUN_SECTIONS]
    page["tabs"].append(rewrite(project))
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
    edges["widgets"].insert(0, {"type": "combo", "id": "view_ts", "label": "显示",
                                "options_from": {"method": "timeset_options", "args": {"all": True}, "field": "timesets"}})
    pattern = tabs["向量"]
    for src in page["sources"]:
        if src["id"] == PREFIX + "edges":
            src["args"] = {"timeset": "$form." + PREFIX + "view_ts"}
            src["interval_ms"] = 1000
    page["sources"].append({"id": PREFIX + "spec_rows", "method": "pe.spec_rows", "interval_ms": 2000})

    for tab in ({"title": "引脚映射", "sections": [pin_map, instruments]},
                {"title": "规格", "sections": [SPEC_SECTION]},
                {"title": "电平", "sections": [levels]},
                {"title": "时序", "sections": [timesets, edges]},
                {"title": "图形编辑", "sections": pattern["sections"]}):
        page["tabs"].append(rewrite(json.loads(json.dumps(tab))))

    with open(PAGE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(page, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
