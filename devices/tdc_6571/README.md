# tdc_6571 — TDC测量（PXIe-6571）

PXIe-6571 上的 TDC 测量。两种用法：

1. **信号生成**：给四个引脚填参数，直接生成图形、预览波形、burst——
   CLK 输出连续时钟；TRIG 检测到由低到高（或由高到低）的边沿后触发；OUT1 在触发后输出指定频率、
   周期数的方波；OUT2 延时一段时间后输出另一路方波；结束后等待，再按次数重复。
2. **运行已有工程**：Digital Pattern Editor 工程 **DemoPrj-TDC** 原样放在 `project/DemoPrj-TDC/`，
   插件读它的引脚映射、规格、电平、时序和编译好的 `.digipat`，解码、分析、修改次数、burst。

这套程序里 6571 是 TDC 的激励源：等芯片 BUSY 引脚拉高（`jump_if` 等待环对 H 比较），发 START，
再发一串 HIT，等待一段时间，整个测量重复 N 次（大多数图形是 60000 次）。

## 页面

| 标签 | 内容 |
|---|---|
| 信号生成 | 选 CLK / TRIG / OUT1 / OUT2 引脚、触发边沿、时钟频率；OUT1、OUT2 的频率、周期数、相对触发的延时；结束后等待、重复次数。生成后显示实际（取整后）的频率与延时、向量数、波形预览和事件表；运行 / 中止 / 保存到工程 |
| 运行 | 选图形；运行 / 单次试跑（外层循环临时改成 1）/ 中止；状态、PASS/FAIL、进度、已用/预计时间、失败计数；解码出的 START→HIT1、HIT 个数/周期/宽度、单次测量时间、循环次数、总时间；第一次测量的时序图和事件表；图形检查提示 |
| 修改图形 | 图形里所有 `repeat` / `set_loop` 次数，带换算后的时间和原注释；回车即改，时序立刻更新；另存为新 `.digipat` 或撤销 |
| 向量 | 解码后的完整向量表：标签、各引脚状态、指令、时序集、周期、注释 |
| 电平 / 时序 | 规格可改（如 `DC.VDD`，改完重新加载电平和时序）；引脚电平（公式按规格求值）；时序集和用到它的图形 |
| 引脚 / 工程 | 引脚映射（仪器、通道、分组）；工程文件；仿真 TDC 的转换时间 |

## 信号生成是怎么做的

- 时钟周期 T = 1 / 时钟频率，所有边沿都取整到 T 的整数倍；取整后的频率和延时会显示出来。
- CLK 用归零（return-to-low）驱动格式，每个向量里都是 1，所以每个周期出一个脉冲，`repeat` 里也不断。
- TRIG 的等待环和 DemoPrj-TDC 里 `WaitForBusy` 用的是同一组指令字（比较向量 + 对自身的 `jump_if`），
  先等相反电平、再等目标电平，所以是真正的边沿触发。
- OUT1 / OUT2 两路方波可以重叠：把两路的所有边沿合并成时间段，每段一个向量加 `repeat`。
- 直接写成编译后的 `.digipat`（以工程里的图形为模板），不需要 Digital Pattern Editor 的编译器；
  同时写一个时序表 `tdcgen_<周期>ps_<CLK>_<TRIG>.digitiming`，只有这个时序集。
- 生成的文件先放在 `_work/gen/`，“保存到工程”后图形进 `Patterns/`、时序表进 `Timing/`；
  参数存在旁边的 `.tdcgen.json` 里。

**真机上第一次用请先单次试跑确认**：图形和时序表都是按解码出的格式生成的，归零格式在时序表里的写法
（`DriveReturnLow` 的 On / Data / Return / Off）按 NI 时序文件格式写，还没有在 8881 上验证过；如果
驱动报时序表格式错误，可以在 Digital Pattern Editor 里打开这个时序表修正后保存，图形不用改。

## 修改图形不需要编译器

`.digipat` 是 TDMS 文件，每个向量的指令是一个 32 位字，`repeat` / `set_loop` 的次数在低 16 位。
插件只改这一个字（文件大小和其余内容不变），所以改延时、HIT 个数、脉宽、循环次数都不用打开
Digital Pattern Editor 重新编译。运行时每次只加载要跑的那一个图形（先 `unload_all_patterns`），
所以另存的新图形可以沿用原图形的起始标签。

次数上限是 65535（编码只有 16 位）。要改引脚状态、加删向量，还是用 Digital Pattern Editor。

## 解码规则（Digital Pattern Editor 21.8 编译格式）

- 引脚状态：每个引脚 4 位，引脚 i 在第 i/2 个字节，偶数引脚在低 4 位；取值同 `nidigital.PinState`：
  `0 1 L(3) H(4) X(5) M V D E`。
- 指令：位 31..28 操作码（0 普通、1 `repeat`、4 `set_loop`、5 `end_loop`、7 `jump_if`、F `halt`），
  位 27..23 时序集序号，位 22..16 标志，位 15..0 操作数（次数或标签序号）。

`jump_if` 的条件编码没有完全解出来，界面上显示原始值（如 `cond 0x28!`）；仿真里把“对 H 比较的自循环
`jump_if`”当作等待 BUSY 拉高。

## 时序分析

插件按指令执行一遍图形：记录第一次外层测量的完整波形，之后的循环按单次时间外推，所以 60000 次的图形
也能瞬间算出总时间。引脚角色自动判断：做过 H/L 比较的是 BUSY；最先出现上升沿的驱动引脚是 START；
其余脉冲最多的是 HIT。BUSY 等待时间来自仿真芯片（默认 120 µs，可在“引脚 / 工程”里改），真机上取决于芯片。

会检查出的可疑写法，例如 `TDC_Mode3` 第 15 个向量 `end_loop(SendHit)` 跳回到它自己的
`set_loop`（v12）之前，第二段 HIT 循环很可能应该是 `end_loop(SendHit2)`。

## 设备配置

```json
{ "name": "tdc1", "plugin": "tdc_6571", "resource": "PXI1Slot15", "simulate": false,
  "options": { "pattern": "TDC_M20_2" } }
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `project` | `project/DemoPrj-TDC` | 工程目录（可指向 Digital Pattern Editor 正在用的目录） |
| `pattern` | 第一个 | 打开时选中的图形 |
| `work_dir` | `_work/` | 运行用的临时图形、改过的规格文件 |
| `busy_delay_us` | 120 | 仿真 TDC 的转换时间 |
| `driver_simulate` | false | 真机模式下改用 NI-Digital 驱动自带的仿真 |

`simulate: true` 用纯 Python 仿真（不需要 NI 驱动）；`false` 需要 8881 上装好 NI-Digital 和
`pip install nidigital`。6571 同一时间只能被一个设备打开，所以 `tdc_6571` 和 `ni_digital` 不要配置在同一个
工位里指向同一块板卡（见 `stations/bench_8881_tdc.json`）。

## 脚本

```python
tdc = dm.device("tdc1")
tdc.select(name="TDC_M20_2")
tdc.set_count(name="v10 set_loop", value=8)      # 8 个 HIT
s = tdc.summary()
dm.log(f"HIT {s['hit_count']} 个，周期 {s['hit_period']*1e6:.2f} µs，总时间 {s['total_text']}")
tdc.burst(single=True)
while tdc.status()["running"]:
    dm.sleep(0.1)
dm.record(pattern=s["pattern"], result=tdc.status()["result"])
```

## 测试

`python -m unittest discover -s devices/tdc_6571/tests`：TDMS 解码与写回、向量和引脚状态、24 个图形的时序分析、
修改次数只改一个字、另存后起始标签不变、规格改动影响电平、burst 进度/中止/单次试跑；信号生成的指令编码、
时钟在每个向量里、生成图形的实测频率/周期数/延时与设定一致、下降沿、取整提示、归零时序表、参数校验。
