"""CSV signal analysis: sampling validation, FFT, and SciPy filters."""
import csv
import io
import json
import math
from pathlib import Path

import numpy as np
from scipy import signal, ndimage
from dm.plugin import Plugin, method

MAX_SAMPLES = 1_000_000


def finite(value, name, minimum=None, maximum=None):
    try: v = float(value)
    except (TypeError, ValueError): raise ValueError(name + "须为数值") from None
    if not math.isfinite(v) or (minimum is not None and v < minimum) or (maximum is not None and v > maximum):
        raise ValueError(name + "超出有效范围")
    return v


def integer(value, name, lo, hi):
    v = finite(value, name, lo, hi)
    if v != int(v): raise ValueError(name + "须为整数")
    return int(v)


def thin_indices(series, limit=6000):
    """Retain extrema of every curve in each time bucket; full data remains in memory."""
    n = len(series[0])
    if n <= limit: return np.arange(n)
    buckets = max(1, limit // (2*len(series)))
    out = {0, n-1}
    edges = np.linspace(0, n, buckets+1, dtype=int)
    for a, b in zip(edges[:-1], edges[1:]):
        for y in series:
            out.add(a+int(np.argmin(y[a:b]))); out.add(a+int(np.argmax(y[a:b])))
    return np.array(sorted(out))


def spectrum(y, fs, window, nfft, detrend):
    n = len(y)
    if nfft == 0: nfft = n
    if nfft < n: raise ValueError("FFT 点数不能小于选中的采样点数；0 表示使用全部采样点")
    if detrend not in ("none", "constant", "linear"): raise ValueError("去趋势类型无效")
    if window not in ("boxcar", "hann", "hamming", "blackman", "blackmanharris", "flattop"):
        raise ValueError("FFT 窗函数无效")
    x = signal.detrend(y, type=detrend) if detrend != "none" else y
    w = signal.get_window(window, n, fftbins=True)
    z = np.fft.rfft(x*w, n=nfft)
    amp = np.abs(z)/w.sum()
    psd = np.abs(z)**2 / (fs*np.sum(w*w))
    stop = -1 if nfft % 2 == 0 else None
    amp[1:stop] *= 2; psd[1:stop] *= 2
    return np.fft.rfftfreq(nfft, 1/fs), amp, psd


def filter_signal(x, fs, c):
    family = c["family"]
    phase = c["phase"]
    if phase not in ("zero", "causal"): raise ValueError("相位模式无效")
    n = len(x)
    b = sos = None
    note = ""
    if family == "none": return x.copy(), None, None, "未使用滤波器"
    if family in ("median", "savgol", "moving_average"):
        size = integer(c["length"], "窗口点数", 3, min(4095, n))
        if size % 2 == 0: raise ValueError("窗口点数须为奇数")
        if family == "median":
            if size > 255: raise ValueError("中值滤波窗口最多 255 点")
            return ndimage.median_filter(x, size=size, mode="reflect"), None, None, "中值滤波：居中非线性窗口，边界反射；没有线性频率响应"
        if family == "savgol":
            degree = integer(c["polyorder"], "多项式阶数", 0, size-1)
            y = signal.savgol_filter(x, size, degree, mode="interp")
            b = signal.savgol_coeffs(size, degree)
            freq, h = signal.freqz(b, worN=2048, fs=fs)
            return y, freq, np.abs(h), "Savitzky–Golay：居中拟合，边缘使用插值；响应图表示内部平稳区间"
        b = np.ones(size)/size
    elif family == "notch":
        f0 = finite(c["cutoff1"], "陷波中心频率", 1e-12, fs/2)
        if f0 >= fs/2: raise ValueError("陷波中心必须小于 Nyquist 频率")
        q = finite(c["q"], "品质因数 Q", .01, 10000)
        b0, a0 = signal.iirnotch(f0, q, fs=fs)
        sos = signal.tf2sos(b0, a0)
    elif family in ("butter", "cheby1", "cheby2", "ellip", "bessel", "fir"):
        kind = c["kind"]
        if kind not in ("lowpass", "highpass", "bandpass", "bandstop"): raise ValueError("滤波类型无效")
        c1 = finite(c["cutoff1"], "截止频率 1", 1e-12, fs/2)
        if c1 >= fs/2: raise ValueError("截止频率必须小于 Nyquist 频率")
        cutoff = c1
        if kind in ("bandpass", "bandstop"):
            c2 = finite(c["cutoff2"], "截止频率 2", 1e-12, fs/2)
            if not c1 < c2 < fs/2: raise ValueError("带通/带阻要求 0 < 截止频率1 < 截止频率2 < Fs/2")
            cutoff = [c1, c2]
        if family == "fir":
            taps = integer(c["length"], "FIR 抽头数", 3, 4095)
            if taps % 2 == 0: raise ValueError("FIR 抽头数须为奇数")
            b = signal.firwin(taps, cutoff, pass_zero=kind, fs=fs, window="hamming")
        else:
            order = integer(c["order"], "IIR 阶数", 1, 20)
            rp = finite(c["rp"], "通带纹波", .001, 100)
            rs = finite(c["rs"], "阻带衰减", .001, 300)
            if family == "ellip" and rs <= rp: raise ValueError("椭圆滤波器阻带衰减须大于通带纹波")
            sos = signal.iirfilter(order, cutoff, rp=rp, rs=rs, btype=kind, ftype=family, fs=fs, output="sos")
    else: raise ValueError("不支持的滤波器：" + str(family))
    if sos is not None:
        pad = 3*(2*len(sos)+1-min(int((sos[:, 2] == 0).sum()), int((sos[:, 5] == 0).sum())))
        if phase == "zero" and n <= pad: raise ValueError(f"零相位滤波至少需要 {pad+1} 点；增加采样点或改用 causal")
        y = signal.sosfiltfilt(sos, x) if phase == "zero" else signal.sosfilt(sos, x)
        freq, h = signal.sosfreqz(sos, worN=2048, fs=fs)
    else:
        pad = 3*len(b)
        if phase == "zero" and n <= pad: raise ValueError(f"零相位 FIR/均值滤波至少需要 {pad+1} 点")
        y = signal.filtfilt(b, [1.0], x) if phase == "zero" else signal.lfilter(b, [1.0], x)
        freq, h = signal.freqz(b, worN=2048, fs=fs)
    gain = np.abs(h)**(2 if phase == "zero" else 1)
    note = "双向零相位：幅频响应为单次滤波的平方，有效阶数加倍；端点存在边界效应" if phase == "zero" else "单向因果滤波：零初始状态，包含启动瞬态及相位/群延迟"
    return y, freq, gain, note


class SignalAnalysis(Plugin):
    def __init__(self, config):
        super().__init__(config)
        self.rows = []; self.headers = []; self.lines = []; self.path = ""
        self.rev = 0; self.full = None; self.result_data = {"rev": 0}
        self.sampling = {"data_column": 1, "time_column": 0, "fs_mode": "manual", "sample_rate": 10000,
                         "time_unit": "s", "start": 0, "count": 0, "gain": 1, "offset": 0, "unit": "V"}
        self.filter = {"family": "butter", "kind": "lowpass", "order": 4, "cutoff1": 500, "cutoff2": 2000,
                       "phase": "zero", "length": 31, "polyorder": 3, "q": 30, "rp": 1, "rs": 60}
        self.fft = {"window": "hann", "nfft": 0, "detrend": "constant", "scale": "psd_db"}
        self.demo()

    def invalidate(self):
        self.rev += 1; self.full = None; self.result_data = {"rev": self.rev}

    @method("CSV 文件预览与数据集信息")
    def info(self):
        return {"path": self.path, "samples": len(self.rows), "columns": len(self.headers),
                "column_names": " / ".join(f"{i}: {h}" for i, h in enumerate(self.headers)),
                "status": "已分析" if self.full else "已导入，请配置采样并分析", "rev": self.rev,
                "preview": [{"line": line, "values": " | ".join(row)} for line, row in zip(self.lines[:15], self.rows[:15])]}

    @method("采样、滤波和 FFT 当前配置")
    def settings(self): return {"sampling": self.sampling, "filter": self.filter, "fft": self.fft}

    @method("CSV 列选项，列编号从 0 开始")
    def column_options(self, time: bool = False):
        values = [{"label": f"{i}: {h}", "value": i} for i, h in enumerate(self.headers)]
        return {"columns": ([{"label": "无时间列", "value": -1}]+values) if time else values}

    @method("导入 CSV；保留原始数据，不跳过无效数值")
    def load_csv(self, path: str, delimiter: str = "auto", header: str = "yes", skip_rows: int = 0, encoding: str = "utf-8-sig"):
        file = Path(path).expanduser()
        if not file.is_file(): raise ValueError("CSV 文件不存在："+str(file))
        if file.stat().st_size > 128*1024*1024: raise ValueError("CSV 文件上限为 128 MiB")
        skip_rows = integer(skip_rows, "跳过行数", 0, MAX_SAMPLES)
        if header not in ("yes", "no"): raise ValueError("表头选项须为 yes 或 no")
        if encoding not in ("utf-8-sig", "gb18030", "utf-16"): raise ValueError("不支持的文件编码")
        text = file.read_text(encoding=encoding)
        physical_lines = text.splitlines(keepends=True)[skip_rows:]
        text = "".join(physical_lines)
        if delimiter == "auto":
            try: delim = csv.Sniffer().sniff(text[:16384], delimiters=",;\t|").delimiter
            except csv.Error: delim = ","
        else:
            delim = {"comma": ",", "semicolon": ";", "tab": "\t", "pipe": "|"}.get(delimiter)
            if delim is None: raise ValueError("CSV 分隔符无效")
        reader = csv.reader(io.StringIO(text), delimiter=delim, strict=True)
        rows, lines, headers = [], [], None
        try:
            for row in reader:
                if not row or all(not v.strip() for v in row): continue
                if headers is None:
                    if not 1 <= len(row) <= 128: raise ValueError("CSV 列数须为 1…128")
                    headers = [v.strip() or f"column_{i}" for i, v in enumerate(row)] if header == "yes" else [f"column_{i}" for i in range(len(row))]
                    if header == "yes": continue
                if len(row) != len(headers): raise ValueError(f"CSV 第 {reader.line_num+skip_rows} 行列数不一致")
                rows.append([v.strip() for v in row]); lines.append(reader.line_num+skip_rows)
                if len(rows) > MAX_SAMPLES: raise ValueError(f"CSV 最多支持 {MAX_SAMPLES} 行数据")
        except csv.Error as exc: raise ValueError("CSV 解析失败："+str(exc)) from None
        if len(rows) < 8: raise ValueError("CSV 至少需要 8 行采样数据")
        self.rows, self.lines, self.headers, self.path = rows, lines, headers, str(file.resolve())
        self.sampling.update(data_column=0, time_column=-1, fs_mode="manual", start=0, count=0)
        self.invalidate()
        self.log(f"已导入 CSV：{len(rows)} 行 / {len(headers)} 列")
        return self.info()

    @method("载入内置双音加噪声示例并分析")
    def demo(self):
        t = np.arange(4096)/10000
        y = np.sin(2*np.pi*50*t)+.4*np.sin(2*np.pi*1500*t)+np.random.default_rng(7).normal(0,.08,len(t))
        self.headers = ["time_s", "signal_V"]
        self.rows = [[repr(a), repr(b)] for a, b in zip(t.tolist(), y.tolist())]
        self.lines = list(range(2, len(t)+2)); self.path = "内置示例：50 Hz + 1500 Hz + 噪声"
        self.sampling.update(data_column=1, time_column=0, fs_mode="manual", sample_rate=10000, start=0, count=0)
        self.invalidate()
        # Always load a runnable demonstration, even after an unrelated filter configuration.
        self.filter.update(family="butter", kind="lowpass", order=4, cutoff1=500, phase="zero")
        self.fft.update(window="hann", nfft=0, detrend="constant", scale="psd_db")
        return self.analyze()

    def selected(self, c):
        if not self.rows: raise ValueError("请先导入 CSV")
        col = integer(c["data_column"], "数据列", 0, len(self.headers)-1)
        tc = integer(c["time_column"], "时间列", -1, len(self.headers)-1)
        start = integer(c["start"], "起始采样点", 0, len(self.rows)-1)
        count = integer(c["count"], "采样点数", 0, len(self.rows))
        stop = len(self.rows) if count == 0 else start+count
        if stop > len(self.rows): raise ValueError("采样范围超出 CSV 数据长度")
        if stop-start < 8: raise ValueError("至少选择 8 个采样点")
        def column(index):
            result = []
            for row, line in zip(self.rows[start:stop], self.lines[start:stop]):
                try: value = float(row[index])
                except ValueError: raise ValueError(f"CSV 第 {line} 行、第 {index} 列不是数值：{row[index]}") from None
                if not math.isfinite(value): raise ValueError(f"CSV 第 {line} 行、第 {index} 列含 NaN/Inf")
                result.append(value)
            return np.asarray(result)
        fs = finite(c["sample_rate"], "采样频率", 1e-9, 1e12)
        if c["fs_mode"] not in ("manual", "time"): raise ValueError("采样频率来源无效")
        time = None
        if tc >= 0:
            if tc == col: raise ValueError("数据列与时间列不能相同")
            scale = {"s": 1, "ms": 1e-3, "us": 1e-6, "ns": 1e-9}.get(c["time_unit"])
            if scale is None: raise ValueError("时间单位无效")
            time = column(tc)*scale
            dt = np.diff(time)
            if np.any(dt <= 0): raise ValueError("时间列必须严格递增，不能重复或倒序")
            step = float(np.median(dt))
            if np.max(np.abs(dt-step))/step > .001: raise ValueError("时间列不是等间隔采样（间隔偏差超过 0.1%）；请先重采样")
            inferred = 1/step
            if c["fs_mode"] == "time": fs = inferred
            elif abs(fs-inferred)/inferred > .001: raise ValueError(f"手动 Fs 与时间列不一致：时间列推算 {inferred:g} Hz；改用 time 或不选择时间列")
        elif c["fs_mode"] == "time": raise ValueError("从时间推算 Fs 时必须选择时间列")
        x = column(col)*finite(c["gain"], "增益")+finite(c["offset"], "偏置")
        if not np.isfinite(x).all(): raise ValueError("增益或偏置导致数据溢出")
        return (np.arange(len(x))/fs if time is None else time-time[0]), x, fs

    @method("应用采样范围、频率、数据列并分析")
    def configure_sampling(self, data_column: int = 0, time_column: int = -1, fs_mode: str = "manual", sample_rate: float = 10000,
                           time_unit: str = "s", start: int = 0, count: int = 0, gain: float = 1, offset: float = 0, unit: str = "V"):
        candidate = dict(data_column=data_column, time_column=time_column, fs_mode=fs_mode, sample_rate=sample_rate,
                         time_unit=time_unit, start=start, count=count, gain=gain, offset=offset, unit=unit)
        self.selected(candidate)  # Commit only after validation.
        old = self.sampling
        self.sampling = candidate
        try: return self.analyze(filter_options={"family": "none"}, fft_options={"nfft": 0})
        except Exception:
            self.sampling = old
            raise

    @method("配置滤波器和 FFT，并计算处理前后结果")
    def analyze(self, filter_options: dict = None, fft_options: dict = None):
        f = dict(self.filter); fft = dict(self.fft)
        for dest, options in ((f, filter_options), (fft, fft_options)):
            if options is not None:
                if not isinstance(options, dict) or set(options)-set(dest): raise ValueError("配置对象包含未知参数")
                dest.update(options)
        t, raw, fs = self.selected(self.sampling)
        filtered, rf, rg, note = filter_signal(raw, fs, f)
        if not np.isfinite(filtered).all(): raise ValueError("滤波结果溢出，请降低阶数或调整截止频率")
        nfft = integer(fft["nfft"], "FFT 点数", 0, 2_097_152)
        freq, a0, p0 = spectrum(raw, fs, fft["window"], nfft, fft["detrend"])
        _, a1, p1 = spectrum(filtered, fs, fft["window"], nfft, fft["detrend"])
        scale = fft["scale"]
        if scale == "amplitude": y0, y1, yu = a0, a1, self.sampling["unit"]
        elif scale == "amplitude_db": y0, y1, yu = 20*np.log10(np.maximum(a0, 1e-15)), 20*np.log10(np.maximum(a1, 1e-15)), "dB re 1 "+self.sampling["unit"]
        elif scale == "psd": y0, y1, yu = p0, p1, self.sampling["unit"]+"²/Hz"
        elif scale == "psd_db": y0, y1, yu = 10*np.log10(np.maximum(p0, 1e-30)), 10*np.log10(np.maximum(p1, 1e-30)), "dB re 1 "+self.sampling["unit"]+"²/Hz"
        else: raise ValueError("频谱显示尺度无效")
        self.rev += 1
        def chart(xs, ys, labels, x_label, x_unit, y_unit):
            idx = thin_indices(ys)
            return {"rev": self.rev, "xs": xs[idx].tolist(), "series": [{"label": label, "values": y[idx].tolist()} for y, label in zip(ys, labels)],
                    "x_label": x_label, "x_unit": x_unit, "y_unit": y_unit, "full_points": len(xs), "shown_points": len(idx)}
        stats = []
        for label, y, amp in (("原始", raw, a0), ("滤波后", filtered, a1)):
            peak = int(np.argmax(amp[1:])+1) if len(amp)>1 else 0
            stats.append({"signal": label, "mean": float(np.mean(y)), "rms": float(np.sqrt(np.mean(y*y))),
                          "std": float(np.std(y)), "pp": float(np.ptp(y)), "peak_frequency": float(freq[peak]), "peak_amplitude": float(amp[peak])})
        peaks, _ = signal.find_peaks(a1)
        peaks = sorted(peaks, key=lambda i: a1[i], reverse=True)[:10]
        result = {"rev": self.rev, "count": len(raw), "sample_rate": fs, "nyquist": fs/2, "duration": len(raw)/fs,
                  "resolution": fs/len(raw), "bin_spacing": fs/(nfft or len(raw)), "filter_note": note,
                  "time": chart(t, [raw, filtered], ["原始", "滤波后"], "时间", "s", self.sampling["unit"]),
                  "spectrum": chart(freq, [y0, y1], ["原始", "滤波后"], "频率", "Hz", yu),
                  "response": {"rev": self.rev}, "stats": stats,
                  "peaks": [{"rank": i+1, "frequency": float(freq[k]), "amplitude": float(a1[k])} for i, k in enumerate(peaks)]}
        if rf is not None:
            result["response"] = chart(rf, [20*np.log10(np.maximum(rg, 1e-15))], ["实际应用的幅频响应"], "频率", "Hz", "dB")
        self.full = {"time": t, "raw": raw, "filtered": filtered, "frequency": freq, "raw_amplitude": a0, "amplitude": a1, "raw_psd": p0, "psd": p1}
        self.filter, self.fft, self.result_data = f, fft, result
        self.log(f"分析完成：{len(raw)} 点 / Fs={fs:g} Hz / {f['family']}")
        return {k: v for k, v in result.items() if k not in ("time", "spectrum", "response")}

    @method("最新分析图表和统计结果")
    def result(self): return self.result_data

    @method("导出全分辨率 CSV 或分析配置 JSON，不覆盖已有文件")
    def export(self, path: str, kind: str = "time"):
        if self.full is None: raise ValueError("请先完成分析")
        if not path.strip(): raise ValueError("请填写导出文件路径")
        if kind not in ("time", "spectrum", "settings"): raise ValueError("导出类型无效")
        dest = Path(path).expanduser(); dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open("x", encoding="utf-8-sig" if kind != "settings" else "utf-8", newline="") as file:
            if kind == "settings": json.dump(dict(self.settings(), source=self.path), file, ensure_ascii=False, indent=2)
            else:
                keys = ["time", "raw", "filtered"] if kind == "time" else ["frequency", "raw_amplitude", "amplitude", "raw_psd", "psd"]
                writer = csv.writer(file); writer.writerow(keys); writer.writerows(zip(*(self.full[k] for k in keys)))
        return {"path": str(dest.resolve())}
