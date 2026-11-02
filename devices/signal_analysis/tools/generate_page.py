import json
from pathlib import Path


def text(id, label, value="", init=None):
    w = {"type": "text", "id": id, "label": label, "value": value, "mono": True}
    if init: w["init"] = {"method": "settings", "field": init}
    return w


def num(id, label, value, lo, hi, unit="", init=None, decimals=3):
    w = {"type": "number", "id": id, "label": label, "value": value, "min": lo, "max": hi, "unit": unit, "decimals": decimals}
    if init: w["init"] = {"method": "settings", "field": init}
    return w


def combo(id, label, options, value=None, init=None):
    w = {"type": "combo", "id": id, "label": label, "options": options, "value": value if value is not None else options[0]}
    if init: w["init"] = {"method": "settings", "field": init}
    return w


def button(label, method, args=None, **extras):
    return dict({"type": "button", "label": label, "style": "primary", "fit": True,
                 "action": {"method": method, "args": args or {}}}, **extras)


def section(title, widgets, span=12, subtitle=""):
    return dict(title=title, widgets=widgets, span=span, subtitle=subtitle)


def read(label, source, field, unit=None):
    w = dict(type="readout", label=label, source=source, field=field)
    if unit is not None: w["unit"] = unit
    return w


def main():
    sources = [{"id": "info", "method": "info", "interval_ms": 3000}, {"id": "analysis", "method": "result", "interval_ms": 5000}]
    csv_widgets = [text("csv_path", "CSV 路径"), combo("delimiter", "分隔符", ["auto", "comma", "semicolon", "tab", "pipe"]),
        combo("header", "有表头", ["yes", "no"]), num("skip_rows", "跳过行数", 0, 0, 1000000, decimals=0),
        combo("encoding", "编码", ["utf-8-sig", "gb18030", "utf-16"]),
        button("导入 CSV", "load_csv", {k: "$form."+v for k, v in {"path": "csv_path", "delimiter": "delimiter", "header": "header", "skip_rows": "skip_rows", "encoding": "encoding"}.items()}, refresh=["info", "analysis"], reload_page=True),
        button("载入双音示例", "demo", refresh=["info", "analysis"], reload_page=True)]
    sampling = []
    for id, label, time in (("data_column", "信号列", False), ("time_column", "时间列", True)):
        w = combo(id, label, [], 1 if not time else 0, "sampling."+id)
        w["options_from"] = {"method": "column_options", "args": {"time": time}, "field": "columns"}
        sampling.append(w)
    sampling += [combo("fs_mode", "Fs 来源", ["manual", "time"], init="sampling.fs_mode"),
        num("sample_rate", "采样频率", 10000, 1e-9, 1e12, "Hz", "sampling.sample_rate", 6),
        combo("time_unit", "时间列单位", ["s", "ms", "us", "ns"], init="sampling.time_unit"),
        num("start", "起始点", 0, 0, 999999, init="sampling.start", decimals=0),
        num("count", "采样点数", 0, 0, 1000000, init="sampling.count", decimals=0),
        num("gain", "数据增益", 1, -1e12, 1e12, init="sampling.gain", decimals=6),
        num("offset", "数据偏置", 0, -1e12, 1e12, init="sampling.offset", decimals=6),
        text("unit", "幅度单位", "V", "sampling.unit")]
    sampling.append(button("应用采样并计算 FFT", "configure_sampling", {w["id"]: "$form."+w["id"] for w in sampling}, refresh=["info", "analysis"], reload_page=True))
    filters = [combo("family", "算法", ["none", "butter", "cheby1", "cheby2", "ellip", "bessel", "fir", "notch", "moving_average", "median", "savgol"], "butter", "filter.family"),
        combo("kind", "通带类型", ["lowpass", "highpass", "bandpass", "bandstop"], init="filter.kind"),
        num("order", "IIR 阶数", 4, 1, 20, init="filter.order", decimals=0),
        num("cutoff1", "截止/中心 1", 500, 1e-9, 1e12, "Hz", "filter.cutoff1"),
        num("cutoff2", "截止频率 2", 2000, 1e-9, 1e12, "Hz", "filter.cutoff2"),
        combo("phase", "相位模式", ["zero", "causal"], init="filter.phase"),
        num("length", "抽头/窗口", 31, 3, 4095, init="filter.length", decimals=0),
        num("polyorder", "拟合阶数", 3, 0, 100, init="filter.polyorder", decimals=0),
        num("q", "陷波 Q", 30, .01, 10000, init="filter.q"),
        num("rp", "通带纹波", 1, .001, 100, "dB", "filter.rp"),
        num("rs", "阻带衰减", 60, .001, 300, "dB", "filter.rs")]
    fft = [combo("window", "窗函数", ["boxcar", "hann", "hamming", "blackman", "blackmanharris", "flattop"], "hann", "fft.window"),
        num("nfft", "FFT 点数", 0, 0, 2097152, init="fft.nfft", decimals=0),
        combo("detrend", "去趋势", ["none", "constant", "linear"], "constant", "fft.detrend"),
        combo("scale", "频谱尺度", ["psd_db", "psd", "amplitude_db", "amplitude"], "psd_db", "fft.scale")]
    run = button("应用滤波并计算 FFT", "analyze", {"filter_options": {w["id"]: "$form."+w["id"] for w in filters},
        "fft_options": {w["id"]: "$form."+w["id"] for w in fft}}, refresh=["analysis", "info"])
    chart = lambda field: {"type": "analysis_chart", "source": "analysis", "field": field, "height": 360,
                            "hint": "请先应用采样配置并分析" if field != "response" else "无滤波或中值滤波没有线性幅频响应"}
    stats = {"type": "table", "source": "analysis", "rows_field": "stats", "columns": [
        {"field": "signal", "label": "数据"}, {"field": "mean", "label": "均值", "digits": 6},
        {"field": "rms", "label": "RMS", "digits": 6}, {"field": "std", "label": "标准差", "digits": 6},
        {"field": "pp", "label": "峰峰值", "digits": 6}, {"field": "peak_frequency", "label": "主峰", "unit": "Hz"},
        {"field": "peak_amplitude", "label": "主峰幅值", "digits": 6}]}
    page = {"title": "信号分析 · FFT与滤波", "subtitle": "CSV 原始数据 · 采样配置 · FFT / PSD · IIR / FIR / 平滑 · QCustomPlot", "columns": 12, "sources": sources, "tabs": [
        {"title": "导入与采样", "sections": [section("CSV 数据导入", csv_widgets, 6, "填写完整文件路径。支持逗号、分号、制表符，UTF-8 / GB18030 / UTF-16。无效数据不会静默丢弃。"),
            section("采样配置", sampling, 6, "列号、起始点从 0 开始，采样点数 0=到文件末尾。manual 手动 Fs；time 从时间列推算。选择时间列时要求等间隔。应用采样会暂时关闭滤波并重置 FFT 点数，请随后设置滤波器。"),
            section("当前数据", [read("文件", "info", "path"), read("数据行数", "info", "samples"), read("列", "info", "column_names"), read("状态", "info", "status")]),
            section("原始文件预览（前 15 行）", [{"type": "table", "source": "info", "rows_field": "preview", "columns": [{"field": "line", "label": "CSV 行号"}, {"field": "values", "label": "各列原始内容"}]}])]},
        {"title": "FFT与滤波", "sections": [section("滤波器", filters, 6, "butter=巴特沃斯，cheby1/2=切比雪夫，ellip=椭圆，bessel=贝塞尔，fir=Hamming窗FIR，notch=陷波。窗口/抽头须为奇数。median 与 savgol 使用固定居中窗口。"),
            section("FFT 设置", fft+[run], 6, "默认 psd_db：单边功率谱密度的 dB 值，10·log10(PSD / 参考密度)，参考为 1 单位²/Hz，不是 dBFS。nfft=0 使用全部采样点，增大只作补零。zero 为双向零相位，causal 为单向滤波。"),
            section("时域对比", [chart("time")]), section("FFT 频谱对比", [chart("spectrum")]), section("滤波器幅频响应", [chart("response")])]},
        {"title": "结果与导出", "sections": [section("采样与频率分辨率", [read("有效采样点", "analysis", "count"), read("采样频率", "analysis", "sample_rate", "Hz"),
            read("Nyquist", "analysis", "nyquist", "Hz"), read("记录时长 N/Fs", "analysis", "duration", "s"),
            read("记录分辨率 Fs/N", "analysis", "resolution", "Hz"), read("FFT 栅格间距", "analysis", "bin_spacing", "Hz")], subtitle="补零可以加密 FFT 栅格，不能增加原始记录的频率分辨能力。"),
            section("统计结果", [stats], subtitle="主峰排除 DC，使用 FFT 栅格峰值；非整周期信号可能有泄漏和幅值栅栏效应。"),
            section("滤波后前十个局部谱峰", [{"type": "table", "source": "analysis", "rows_field": "peaks", "columns": [{"field": "rank", "label": "序号"}, {"field": "frequency", "label": "频率", "unit": "Hz"}, {"field": "amplitude", "label": "峰值幅度", "digits": 6}]}]),
            section("导出", [text("export_path", "输出路径"), combo("export_kind", "内容", ["time", "spectrum", "settings"]),
                button("导出结果", "export", {"path": "$form.export_path", "kind": "$form.export_kind"}, publish="exported"), read("已导出", "exported", "path")], subtitle="time：全分辨率时域 CSV；spectrum：完整频谱 CSV（幅度和 PSD）；settings：配置 JSON。目标文件须不存在。绘图会压缩显示点数，计算和导出始终使用完整数据。")]}]}
    (Path(__file__).resolve().parents[1]/"page.json").write_text(json.dumps(page, ensure_ascii=False, indent=1)+"\n", encoding="utf-8")


if __name__ == "__main__": main()
