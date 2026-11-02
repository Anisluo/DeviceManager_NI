# 10 万点性能验收

硬约束：鼠标操作延迟 ≤100 ms，FFT 处理 ≤1000 ms。**比较最大值，不以均值/P95 代替最大值**；超限时测试返回非零。

## 运行

```powershell
cmake --build build --target analysis_performance_test analysis_plot_test --parallel 4
ctest --test-dir build -L acceptance --output-on-failure -V
```

使用 Release 构建，Qt/Python 运行库须可用。性能测试被标记为 `RUN_SERIAL`，不与其他 CTest 并发运行。
它们不会截取或操作用户桌面。CPU、系统负载、图表尺寸和渲染后端都会影响实测结果，因此其他机器也应运行验收。

## 鼠标：事件到完成帧提交

`DeviceManager/tests/analysis_performance_test.cpp` 通过真正的 `QQuickWindow` 分发鼠标事件，并等待：

1. 若操作改变范围，所需版本的 QCustomPlot 缓存已完成；
2. 对应缓存/选框已由 Quick 绘制；
3. `frameSwapped` 已发出。

不是只统计输入处理函数耗时，也不把旧图像提前提交算作成功。
测试使用离屏 Qt Quick software/basic 渲染，窗口 1600×1080，同时保留三个 QCustomPlot 图表。
时域每曲线 100000 点，频谱和第三图每曲线各 50001 点，每图两条曲线，**不使用生产页面的 6000 点显示压缩**。
分别测量 X/Y 选轴、X/Y 滚轮、框选移动、框选松开、右键平移、双击复位；每种 20 次，选框移动 60 次。
同时校验选轴后另一轴不变化，以及选框移动期间没有整图重绘。

测量从应用收到事件开始，到 Qt 完成帧提交结束；不包含 OS 输入队列等待、桌面合成器延迟或物理显示器扫描输出。
因此这是应用内可复现的验收，不是显示器光学延迟测试，也不是任意系统满载状态下的实时性保证。

报告：`build/performance/analysis_mouse.json`，包含每次样本、最大值、P95、尺寸、点数和渲染后端。

## FFT：核心和完整流水线分别检查

`tools/accept_performance.py` 生成并导入 100000 点、Fs=200000 Hz 的双音 CSV。
执行 10 次 Hann 窗、去均值、100000 点 rFFT，并检查已知峰值频率、幅度和 PSD 积分，防止只追求速度而丢数据或算错。

同时执行 10 次完整插件调用：选列/时间均匀性检查、4 阶 Butterworth 双向滤波、原始与处理后两次 FFT、PSD、统计、保留极值的图表数据以及两次插件 API JSON 返回。
另检查所有可用滤波器各一次完整调用，参数为 IIR 原型 4 阶，FIR/均值/中值/拟合窗口 31 点，Savitzky–Golay 3 阶，陷波 Q=30。
FFT 本身与所有这些完整调用均须 ≤1000 ms。

不计 Python/SciPy 首次导入、插件创建、CSV 生成/文件读取；CSV 导入时间另行记录。首个 10 万点 FFT 不被丢弃。
该验收不声称覆盖所有极端滤波参数或最大 2097152 点补零；例如高抽头 FIR 双向滤波的初始条件求解、超大中值窗口可能成为额外瓶颈。

报告：`build/performance/signal_analysis_fft.json`，记录全部样本、NumPy/SciPy/Python 版本、各滤波器耗时及测试配置。

## 本机结果

测试日期：2026-11-02。Intel Core i5-10400F（6 核 / 12 线程），Windows，Qt 6.8.3，MinGW Release。
开发中的首轮压力测试曾出现框选松开约 144 ms、平移约 122 ms，瓶颈在曲线抗锯齿重绘和图像提交，而非 FFT。
调整为快速非抗锯齿折线、8 ms 合并刷新和独立选轴高亮后：

| 指标 | 实测最大值（最终验收） | 预算 |
|---|---:|---:|
| 选轴 | 8.50 ms | 100 ms |
| 选框移动 | 7.58 ms | 100 ms |
| X/Y 滚轮 | 21.99 ms | 100 ms |
| 框选松开 | 18.93 ms | 100 ms |
| 平移 | 20.71 ms | 100 ms |
| 复位 | 22.32 ms | 100 ms |
| 单次 FFT | 5.82 ms | 1000 ms |
| 默认完整分析流水线 | 144.93 ms | 1000 ms |
| 各滤波器完整分析（文中参数） | 146.43 ms | 1000 ms |

每次运行会更新 JSON，实际验收以本次 JSON 和 CTest 退出状态为准。`analysis_qcustomplot` 另外保留缩放正确性、位图缓存和 200 次选框移动仅一次整图重绘的回归检查。
