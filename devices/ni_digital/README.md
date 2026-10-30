# ni_digital — PXIe-6571 数字图形仪器

Python 插件。SWD 协议、开短路/漏电算法和结果格式都只有一份代码，下面接两个后端：

| 后端 | 用途 | 依赖 |
|---|---|---|
| `driver` | PXIe-8881 上的真实 6571；`simulate: true` 时由 NI 驱动模拟仪器 | NI-Digital Pattern Driver、`pip install nidigital hightime` |
| `sim` | 纯 Python：6571 + 一颗虚拟 Cortex-M0 测试芯片 | 无 |
| `auto`（默认） | 能打开驱动就用 `driver`，否则退回 `sim` 并在日志里说明 | — |

## 文件

```
ni_digital.py      插件：方法定义、开短路/漏电、SWD 两种物理层
swd.py             ARM ADIv5 SWD 主机协议（DP/AP、内存、Cortex-M 内核控制）
driver_backend.py  nidigital 后端；生成 pinmap 和 SWD source/capture 图形
sim_backend.py     仿真 6571：电平、静态 I/O、PPMU、burst、图形 SWD 的时序窗口
sim_dut.py         虚拟芯片：引脚电气模型、SWD 位级状态机、存储器映射、trim 寄存器
pattern_core.py    加载 tdc_measure 的 DLL，转发图形工程编辑器的 pe.* 方法
tests/             python -m unittest discover -s devices/ni_digital/tests
```

## 工位配置

```json
{ "name": "dig1", "plugin": "ni_digital", "resource": "PXI1Slot15", "simulate": false,
  "options": {
    "backend": "driver",
    "vdd": 3.3,
    "pins":   { "SWCLK": 0, "SWDIO": 1, "nRST": 2, "PA0": 4, "PA1": 5 },
    "groups": { "SWD": ["SWCLK", "SWDIO"], "GPIO": ["PA0", "PA1"] },
    "pulls":  { "SWDIO": "up", "nRST": "up", "SWCLK": "down" },
    "expect_idcode": "0x0BB11477",
    "swd_phy": "static", "swd_frequency": 1e6,
    "pattern_compiler": "C:\\Program Files\\National Instruments\\...\\DigitalPatternCompiler.exe",
    "sim": { "seed": 7, "vdd": 3.3, "faults": { "PA2": "short_gnd" } }
  } }
```

* `pins` 生成单站点 pinmap（`_generated/station.pinmap`），也可以用 `load_project` 加载自己的工程。
* `pulls` 告诉漏电测试哪些引脚有内部上/下拉：上拉引脚只判 IIH，下拉引脚只判 IIL。
* 仿真故障：`open`、`short_gnd`、`short_vdd`、`short:<另一个引脚>`、`leaky`，运行中也可用 `sim_inject_fault` 注入。

## 功能与方法

| 类别 | 方法 |
|---|---|
| 信息 | `get_info` `list_pins` `pin_table` `reset` |
| 电平 / 时序 / 静态 | `set_levels(pins, vil, vih, vol, voh, vterm)` `get_levels` `set_period` `write_static(pins, 0/1/X)` `read_static` |
| PPMU | `ppmu_force_voltage(pins, voltage, current_range)` `ppmu_force_current(pins, current, v_low, v_high)` `ppmu_measure(pins, what)` `ppmu_off` |
| 开短路 | `continuity(pins, current=-100 µA, low=-0.95, high=-0.25)`：被测引脚灌 -100 µA，其余引脚 PPMU 0 V（这样能发现引脚间短路），量 ESD 二极管压降。逐脚进行，钳位判开路，接近 0 V 判短路 |
| 漏电 | `leakage(pins, v_low, v_high, limit)`：PPMU 分别加 VIL/VIH 测电流；电流达到量程会标 `clamped` |
| SWD | `swd_configure(phy, frequency)` `swd_connect` `swd_check` `swd_read_dp/ap` `swd_write_dp/ap` |
| 存储器 | `mem_read(address, count)` `mem_write(address, value / values, verify)`（每次访问后检查 STICKYERR，总线错误会报出来） |
| 内核 | `core_halt` `core_resume` `core_reset(halt)` `core_status` `core_regs` `core_write_reg` |
| 图形 | `load_project(folder)` `load_pattern(path)` `burst(start_label)`（每引脚失败计数）`list_patterns` |
| 仅仿真 | `sim_inject_fault` `sim_set_vdd` `sim_vref` |

地址和数值可以写成 `"0x2000_0000"` 这样的字符串。

### SWD 的两种物理层

* **static**：每个 SWCLK 周期调用 3~4 次 `write_static`/`read_static`。速度只有几 kHz，但只依赖最基本的驱动 API，不需要图形编译器，首片点亮先用它。
* **pattern**：每种传输形状（例如"8 位请求 + 转向 + 3 位 ACK"）生成一个 `.digipatsrc`，写入位来自 source 波形，读出位进入 capture 波形；首次使用时编译并缓存，之后只换 source 数据。SWCLK 用 RL 格式在 T/2 上升，SWDIO 用 NR 格式，选通在 0.9T。

## 上机验证清单（PXIe-8881 + 6571）

代码中标了 `VERIFY` 的地方是按 nimi-python 文档写的，还没在真机上跑过。建议按以下顺序确认：

1. 插件页"概览"显示"NI-Digital 驱动"后端，引脚表能读到比较器状态。
2. `continuity` / `leakage` 的结果顺序与引脚对应（`ppmu_measure` 的返回顺序）。
3. 用 static PHY 执行 `swd_connect` 读到 IDCODE：这一步验证接线、电平和协议。
4. 确认 `DigitalPatternCompiler.exe` 的路径和命令行参数（`options.pattern_compiler` / `options.compile_args`）。
5. 换成 pattern PHY 再连一次；如果 capture 位不对，先查 `driver_backend.CAPTURE_STATE`（捕获向量用 X 还是 V），再查 SWDIO 的选通时间。
6. 跑 Shmoo 标签页 / `sequences/swd_vdd_shmoo.py`，结果应与仿真形状相似。

## 页面

标签页：概览（仪器、引脚状态、电平、静态驱动）· PPMU（FV/FI、读数和趋势）· 开短路/漏电 · SWD（连接、存储器、内核）· 图形（工程、图形、burst、失败计数）· Shmoo（VIH × SWCLK）· 工程 · 引脚映射 · 规格 · 电平 · 时序 · 向量 Vector。

## 图形工程编辑器

“工程”之后的标签按 Digital Pattern Editor 的文档组织：引脚映射、规格、电平、时序、向量。都是电子表格，操作和 Excel 一样：
拖动或 Shift 选区，打字改值，Ctrl+C / Ctrl+V 可与 Excel 互贴，右键拖动框选后批量修改，图形可 Ctrl+Z 撤销。可以新建或打开工程，
单次试跑，然后**导出为插件**。

- 工程模型只有一份 C++ 代码，在 `tdc_measure` 里。`plugin.json` 的 `"uses": ["tdc_measure"]` 让主机把它的路径传进来，
  `pattern_core.py` 用 ctypes 加载 DLL（的副本），`pe.<方法>` 转发给它；要运行图形时 DLL 再回调本驱动的 `pattern_*` 方法。
  第一次用到时才加载，没有这个 DLL 时驱动其余功能不受影响。
- 工程在 `_work/projects/` 下，第一次打开时复制一份 DemoPrj-TDC 作为默认工程，不影响 TDC测量 自己的工程。“新建工程”生成 8 个引脚、一个时序集、一个 `main` 图形。新工程没有
  `.digiproj`，Digital Pattern Editor 可以打开其中的单个表，但不能当工程打开。
- 导出生成 `devices/<id>/`：运行器 DLL 的副本（改名为 `<id>.dll`）、`plugin.json`、`page.json`、`README.md` 和工程副本，
  不需要编译器。点“重新扫描插件”后出现在侧栏“插件”里，自动用检测到的 6571 运行。再次导出同一个 id 会替换它。
- 这些标签由 `tools/sync_editor_page.py` 从 `tdc_measure/page.json` 生成（规格表写在脚本里），改那边的页面后重新运行一次。

相关序列：`sequences/m0_bringup.py`（开短路 → 漏电 → SWD → SRAM march → 基准 trim 闭环，生成逐项测试记录）、`sequences/swd_vdd_shmoo.py`。
