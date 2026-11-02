# 信号分析 · FFT 与滤波

独立 Python 任务插件，自动出现在侧栏。波形、频谱和幅频响应使用 **QCustomPlot 2.1.1**，通过主程序的 `AnalysisPlot` 接入 QML。
本机已安装 NumPy / SciPy；其他部署机器需在宿主使用的 Python 环境安装 `requirements.txt`。

## 操作

1. 在“导入与采样”输入 CSV 完整路径，设置分隔符、编码、表头以及跳过行数，点击导入。
2. 选择信号列、可选时间列，配置采样频率 Fs、起始点和采样点数。列号及起始点从 0 开始，点数 0 表示直到末尾。
   `manual` 使用手动 Fs，`time` 从时间列推算。时间单位可为 s / ms / us / ns。
   应用采样后关闭滤波并重置 FFT 点数，可先检查原始频谱。
3. 在“FFT与滤波”选择滤波器和 FFT 参数，点击“应用滤波并计算 FFT”，查看时域/频域对比和滤波器响应。
4. 在“结果与导出”查看统计、谱峰及频率分辨率，导出完整时域 CSV、频谱 CSV 或配置 JSON。目标文件须不存在。

无需文件也可使用内置 `50 Hz + 1500 Hz + 噪声` 示例。
FFT 默认显示单边功率谱密度的 dB 值，横轴 Hz，纵轴 dB re 1 单位²/Hz。
每张图上方可分别勾选原始/滤波后等曲线，只绘制勾选内容。选择在切换标签和重新分析后保留；允许全部取消。
取消勾选不修改分析数据和导出结果，也不重算 FFT；“自适应”只按勾选曲线调整范围。
点击 X 轴后滚轮只缩放 X，点击 Y 轴后只缩放 Y；选中轴高亮，再次点击取消选择。

## 滤波器

| 算法 | 配置 |
|---|---|
| none | 不滤波 |
| butter / cheby1 / cheby2 / ellip / bessel | 巴特沃斯、切比雪夫 I/II、椭圆、贝塞尔；低通/高通/带通/带阻；IIR 阶数 1–20 |
| fir | Hamming 窗 FIR，同样支持四种通带类型；抽头数 3–4095 且为奇数 |
| notch | 中心频率和 Q，用于电源工频等窄带干扰 |
| moving_average | 均值滤波，奇数窗口 |
| median | 居中中值滤波，反射边界；窗口 3–255，无线性幅频响应 |
| savgol | Savitzky–Golay 居中多项式拟合，窗口须大于拟合阶数 |

IIR 用二阶节 SOS 实现。`zero` 为双向零相位，实际幅频响应为单次的平方，有效阶数加倍；`causal` 为单向、零初始状态，包含延迟和启动瞬态。
median/savgol 为固定居中处理，不使用 phase 设置。零相位记录太短时明确拒绝，不静默切换算法。
单端截止频率必须处于 `(0, Fs/2)`；带通/带阻还要求截止1小于截止2。带通/带阻 IIR 的单次实际阶数为所填原型阶数的两倍。

## FFT 与数据约束

- 实数单边谱，DC/Nyquist 不加倍，其余正频率加倍。幅度按窗函数 coherent gain 归一化，正弦谱线单位为峰值幅度。
- PSD 使用 `|FFT|² / (Fs × sum(window²))`，单位为幅度单位²/Hz；支持线性及 dB 显示。
- amplitude_db 相对 1 个幅度单位，PSD dB 相对 1 单位²/Hz，**不是 dBFS**。显示底限分别为 -300 dB。
- 窗函数：矩形、Hann、Hamming、Blackman、Blackman-Harris、Flat-top。支持不去趋势、去均值和去线性趋势。
- FFT 点数 0=选定记录长度；可以补零但不能截断所选数据。FFT 栅格间距 `Fs/NFFT` 不等于记录分辨率 `Fs/N`。
- CSV 最大 128 MiB、100 万行、128 列；空白整行忽略，所选列中的空值/非数值/NaN/Inf 明确报出原始文件行号，不自动补点。
- 如果选择时间列，则必须严格递增、采样间隔偏差不超过 0.1%；手动 Fs 也必须与它相符。非均匀采样数据需先重采样。
- 时间图以选定记录首点为 0。增益/偏置只作用于选定信号，源 CSV 不会被修改。
- 绘图传输使用多曲线分桶保留极值（约 6000 点），QCustomPlot 继续自适应采样；全部计算和导出保持完整分辨率。
- 主峰排除 DC；谱峰为 FFT 栅格峰值，没有亚栅格插值，非整周期信号仍可能存在泄漏和栅栏效应。

## 文件与验证

- `signal_analysis.py`：CSV、采样校验、滤波、FFT、统计和导出。
- `page.json`：页面；`tools/generate_page.py` 为生成脚本。
- `tests/test_signal_analysis.py`：FFT 已知幅度、滤波衰减、CSV 编码/列/时间、导出完整性等测试。
- 主程序 `src/app/AnalysisPlot.*`：QCustomPlot 图表；所有 QWidget 操作留在 GUI 线程，Quick 渲染线程仅绘制缓存图像。

测试：`python -m unittest discover -s devices/signal_analysis/tests -v`。
QCustomPlot 集成测试：`ctest --test-dir build -R analysis_qcustomplot --output-on-failure`。

性能验收（10 万点）：`ctest --test-dir build -L acceptance --output-on-failure -V`。
会严格检查每个鼠标操作样本不超过 100 ms、FFT 和所列参数下的完整分析不超过 1000 ms，超限返回失败。
测试方法、参数范围及本机结果见 `PERFORMANCE.md`；JSON 报告位于 `build/performance/`。

算法参考：[NumPy rFFT](https://numpy.org/doc/stable/reference/generated/numpy.fft.rfft.html)、
[SciPy 二阶节双向滤波](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.sosfiltfilt.html)、
[SciPy 滤波器设计](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.iirfilter.html)。
