# tdc_measure — TDC测量（测试任务）

测试任务插件（C++ DLL，`category: "task"`）：在主机检测到的 **PXIe-6571** 上跑 TDC 激励。
插件本身不打开仪器——所有仪器操作都通过主机调用那台 6571 设备（驱动 `ni_digital`）完成，
所以真机和仿真机箱用的是同一个 DLL。

两种用法：

1. **信号生成**：给四个引脚填参数，直接生成图形、预览波形、burst——
   CLK 输出连续时钟；TRIG 检测到由低到高（或由高到低）的边沿后触发；OUT1 在触发后输出指定频率、
   周期数的方波；OUT2 延时一段时间后输出另一路方波；结束后等待，再按次数重复。
2. **运行已有工程**：Digital Pattern Editor 工程 **DemoPrj-TDC** 原样放在 `project/DemoPrj-TDC/`，
   插件读它的引脚映射、规格、电平、时序和编译好的 `.digipat`，解码、分析、修改次数、burst。

## 页面

| 标签 | 内容 |
|---|---|
| 信号生成 | 选 CLK / TRIG / OUT1 / OUT2 引脚、触发边沿、时钟频率；OUT1、OUT2 的频率、周期数、相对触发的延时；结束后等待、重复次数。生成后显示实际（取整后）的频率与延时、向量数、波形预览和事件表；运行 / 中止 / 保存到工程 |
| 运行 | 选图形；运行 / 单次试跑（外层循环临时改成 1）/ 中止；状态、PASS/FAIL、进度、已用/预计时间、失败计数；START→HIT1、HIT 个数/周期/宽度、单次测量时间、循环次数、总时间；第一次测量的时序图和事件表；工程检查和图形提示 |
| 修改图形 | 所有 `repeat` / `set_loop` 次数，带换算后的时间和原注释；回车即改，时序立刻更新；另存为新 `.digipat` 或撤销 |
| 向量 | 电子表格：标签、时序集、指令、各引脚状态、注释。拖动或右键框选后批量修改，Ctrl+C / Ctrl+V 与 Excel 互贴，插入 / 删除行，增减引脚列，Ctrl+Z 撤销；保存、另存为；本图形用到的时序边沿 |
| 电平 / 时序 | 规格可改（如 `DC.VDD`，改完重新加载电平和时序）；时序集的周期、新建、删除；每个引脚 / 引脚组的驱动格式和 On / Data / Return / Off / Strobe；引脚电平和端接（都可写规格公式） |
| 引脚 / 工程 | 引脚映射（仪器、通道、分组、增删引脚）；工程文件；还原工程文件；仿真 TDC 的转换时间 |

## 手动触发（仿真）

“运行”和“信号生成”标签里的 **TRIG 激励（仿真）**：打开“手动触发”后点运行，图形停在 TRIG 等待循环
（`jump_if` 等 H）里不往下走；点“给 TRIG 上升沿 ↑”就执行一次测量，然后回到等待循环。“连续触发 N 次”一次
排队 N 个沿。测量进行中给的沿会被错过（图形不在等待循环里），计入“错过”。关掉手动触发时按原来的方式：
芯片转换完 BUSY 自动拉高。只在仿真里有效，真机上 TRIG 由外部信号驱动。

## 作为运行器

这个 DLL 也是通用的图形运行器：`ni_digital` 的工程编辑器加载它，“导出为插件”复制它，改名后配上 `plugin.json`、`page.json`
和工程副本就是一个新的测试任务（`project_new`、`pattern_new`、`export_plugin`）。导出的页面去掉了“信号生成”，其余相同。
DLL 里的清单仍写着 `tdc_measure`，主机以 `plugin.json` 为准。

## 用到的仪器

`plugin.json` 的 `"requires": {"digital": {"models": ["PXIe-6571"]}}`：主机在检测到的设备里找第一块
PXIe-6571，把设备名写进配置的 `instruments.digital`。没有检测到时页面照常可以看、分析、生成，只是不能运行。

对 6571 的调用（`ni_digital` 驱动提供）：

| 方法 | 用途 |
|---|---|
| `pattern_project(pinmap, specs, levels, timing)` | 第一次运行前加载工程的引脚映射、规格、电平和所有时序表 |
| `pattern_timing(path)` | 应用图形所用时序集所在的时序表（生成的图形有自己的时序表） |
| `pattern_specs(path)` | 改了规格后换规格文件、重新应用电平和时序 |
| `pattern_start(path, label, sim_seconds)` | 只加载这一个图形并 burst，不等待；仿真的 6571 按 `sim_seconds` 计时 |
| `pattern_status(pins)` / `pattern_abort()` | 轮询是否结束、PASS/FAIL 和失败计数；中止 |

加载工程会替换 6571 原来的引脚映射，`ni_digital` 页面上的 SWD 功能之后需要重新初始化。

## 信号生成是怎么做的

- 时钟周期 T = 1 / 时钟频率，所有边沿取整到 T 的整数倍，取整后的频率和延时会显示出来。
- CLK 用归零（return-to-low）驱动格式，每个向量里都是 1，所以每个周期一个脉冲，`repeat` 里也不断。
- TRIG 的等待环和 DemoPrj-TDC 里 `WaitForBusy` 用同一组指令字（比较向量 + 对自身的 `jump_if`），
  先等相反电平、再等目标电平，所以是边沿触发。
- OUT1 / OUT2 可以重叠：两路的所有边沿合并成时间段，每段一个向量加 `repeat`。
- 直接写编译后的 `.digipat`（以工程里的图形为模板），不需要图形编译器；同时写一个时序表
  `tdcgen_<周期>ps_<CLK>_<TRIG>.digitiming`。生成的文件先放在 `_work/gen/`，“保存到工程”后进
  `Patterns/` 和 `Timing/`，参数存在旁边的 `.tdcgen.json`。

CLK 的归零格式按 Digital Pattern Editor 保存的写法写成 `<ReturnToLow>`（On / Data / Return / Off，和工程里
`TDC.digitiming` 的 `pulse_2us` 一样）。0.2 版写的是 `<DriveReturnLow>`，驱动不认；旧的生成时序表请重新生成。
真机上第一次用仍建议先单次试跑确认。

## 时序按驱动格式计算

读时序表时读出每个 `<PinEdge>` 的驱动格式和边沿；引脚组（如 `AllPins`）展开到引脚，引脚自己的条目优先。
周期和边沿可以是规格公式（如 `Time.period*2`），按页面上当前的规格求值。时序图和 START / HIT 分析按格式
展开：

| 格式 | 状态 1 | 状态 0 |
|---|---|---|
| NR `DriveNonReturn` | Data 处变高，保持 | Data 处变低，保持 |
| RL `ReturnToLow` | 每个周期 Data 拉高、Return 回低（`repeat` 里每周期一个脉冲） | 低 |
| RH `ReturnToHigh` | 高 | 每个周期 Data 拉低、Return 回高 |
| SBC `SurroundByComplement` | 每个周期 On 起取反、Data 起为 1、Return 起再取反 | 同理 |

例如 `TDC_1017` 的 I1 在 `pulse_2us` 里是 RL（Data 1 µs，Return 2 µs），`repeat(500)` 是 500 个 1 µs 宽的
脉冲，不是一段 1 ms 的高电平。每周期都有脉冲、太密画不下的引脚（如生成图形的 CLK）在时序图上按状态电平画，
通道名里注明。

## 工程检查

像编辑器编译 / 运行前那样检查当前图形（“运行”标签的“检查”，同时并入提示）：

- 图形引脚不在引脚映射里，或没有连到仪器通道；
- 引脚在电平表里没有电平（运行时沿用仪器当前电平），例如 `TDC` 的 `start`；
- 图形用到的时序集没有定义某个图形引脚的边沿，例如 `TDC_2_T` 的 `working` 只定义了 I1；
- 周期 / 边沿公式无法求值，RL / RH 的 Return 不晚于 Data，边沿或 Strobe 超出周期。

## 修改图形不需要编译器

`.digipat` 是 TDMS 文件，每个向量的指令是一个 32 位字，`repeat` / `set_loop` 的次数在低 16 位。
只改次数时只改这一个字（文件大小和其余内容不变）。每次运行只加载要跑的那一个图形，所以另存的新图形可以
沿用原图形的起始标签。次数上限 65535。

“向量”标签改的是内存里的草稿：运行用草稿的副本（`_work/run/`），“保存到当前图形”或“保存为新图形”才写文件，
这时按解码后的向量表重写 `Pattern`、`PinItem`、`TimeSet` 组，其余属性沿用原文件。标签按序号引用，所以插入、
删除向量不影响 `end_loop` / `jump_if`；被删向量上的标签移到下一个向量，被引用的标签和起始标签不能删。

编译格式是从工程文件解析出来的，不是 NI 公开的格式：重写后的图形在真机上第一次用，先单次试跑。

## 向量语法校验

规则来自 [NI-Digital Pattern User Manual](https://www.ni.com/docs/en-US/bundle/ni-digital-pattern/page/opcodes.html)（`src/syntax.*`）。

- **输入时拒绝**：opcode 名称和参数（`repeat`、`set_loop` 为 1…65535 或 `reg0`…`reg15`）、`jump_if` 条件（`[!]matched`、`[!]failed`、
  `[!]seqflag0…3`、`[!]trig0…3`）、标签必须存在、引脚状态 `0 1 L H X M V D E`；名称必须以字母或下划线开头、只含 ASCII 字母数字下划线、
  不能是保留字、标签不能重复。
- **表格下方列出**：最后一个向量必须是 `halt`、`set_loop` 与 `end_loop` 配对且标签位置正确（错误）；循环嵌套超过 8 层、`matched`
  读取的正好 80 个周期前的向量没有 `match`、`match` 向量没有比较状态（提醒，语法上合法）。向量的 `match` 和 `jump_if` 的自循环
  按手册的 match pipeline 处理。

`.digipat` 的编码是从工程文件解析的，不是 NI 公开格式：`0x20` 按 `match` 显示是推断（工程里所有比较向量都带它，反之亦然）。
`jump`、`call`、`capture` 等语言合法但工程里没有样例的 opcode，编码未确认，校验时明确提示不能写入，要用 Digital Pattern Editor 编译。

## 编辑工程文件

时序集、电平和引脚映射回车后直接写回工程里的 `.digitiming` / `.digilevels` / `.pinmap`（`src/sheets.*`），
只替换改动的元素，其余内容和格式不变，Digital Pattern Editor 可以照常打开。周期、边沿和电平可写数值或规格
公式，写入前先求值。下一次运行时重新把工程加载进 6571；图形运行中不能修改。

被图形用到的时序集、引脚不能删除；第一次修改某个文件时原文件存到 `_work/backup/`，“还原工程文件”放回去。
引脚和时序集改名时，时序表、电平表和所有用到它的图形一起改，有未保存修改的图形要先保存。

## 解码规则（Digital Pattern Editor 21.8 编译格式）

- 引脚状态：每个引脚 4 位，引脚 i 在第 i/2 个字节，偶数引脚在低 4 位；取值同 `nidigital.PinState`：
  `0 1 L(3) H(4) X(5) M V D E`。
- `jump_if` 的标志 `0x28` 加操作数位 15 就是 `!matched`（比较不匹配时跳转），显示和输入都用这个写法。工程里没有别的条件，其他标志按数字显示，如 `jump_if(0x28, 标签)`。
- 指令：位 31..28 操作码（0 普通、1 `repeat`、4 `set_loop`、5 `end_loop`、7 `jump_if`、F `halt`），
  位 27..23 时序集序号，位 22..16 标志，位 15..0 操作数（次数或标签序号）。

插件按指令执行一遍图形：记录第一次外层测量的完整波形，之后的循环按单次时间外推。引脚角色自动判断：做过
H/L 比较的是 BUSY；最先出现上升沿的驱动引脚是 START；其余脉冲最多的是 HIT（生成的图形直接用设定的角色）。
会检查出可疑写法，例如 `TDC_Mode3` 第 15 个向量 `end_loop(SendHit)` 跳回到它自己的 `set_loop` 之前。

## 源码

| 文件 | 内容 |
|---|---|
| `src/tdc_task.cpp` | 插件方法、运行状态、对 6571 的主机调用 |
| `src/project.*` | 工程文件、图形解码与编辑、解释执行、时序分析 |
| `src/sheets.*` | 保留原格式地编辑时序表、电平表、引脚映射 |
| `src/workspace.*` | 新建工程、把工程导出为插件 |
| `src/tdcgen.*` | 四引脚信号生成、时序表 |
| `src/tdms.*` | TDMS 读写与原位修改 |
| `src/util.*` | XML、带单位的数值与公式、文件 |

## 测试

`ctest -R tdc_measure_task`：经 C ABI 加载 DLL，测试程序扮演主机和 6571——解码与时序分析、时序表边沿
（RL 脉冲展开、周期公式随规格变化）、工程检查、修改次数只改一个字、另存、规格、通过主机 burst / 中止 /
单次试跑、信号生成（角色、频率、延时、等待指令、`<ReturnToLow>` 时序表、参数校验、保存）、没有 6571 时的提示。
