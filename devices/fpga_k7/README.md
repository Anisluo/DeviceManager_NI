# fpga_k7 — XC7K480T 虚拟芯片平台（流片前）

芯片的数字 RTL（含 Cortex-M0 + SWD）跑在 Kintex-7 板上，PXIe-6571 以 **1.8 V** 通过 SWD/SPI/GPIO 直连 FPGA 管脚，测试方法和将来测真芯片完全一样。本插件负责旁边的"控制面"：

| 通道 | 用途 |
|---|---|
| **JTAG**（Vivado / Vivado Lab 的 hw_server，装在 8881 上） | 下载 bitstream、配置状态、XADC 温度与电压；可选 JTAG-to-AXI 读写寄存器 |
| **UART 寄存器桥**（`rtl/dm_uart_bridge.v`，921600 8N1） | 平台寄存器：虚拟芯片复位与时钟、模式引脚、6571 SWCLK 计数、trim 镜像、探针 |
| **模拟量桩**（上位机计算） | RTL 里没有模拟电路，插件按可配置的模型把芯片的 trim 码换算成 VREF / VLDO / FOSC，使 trim 闭环和 datasheet 流程在流片前就能跑 |

## 文件

```
fpga_k7.py        插件
uart_link.py      UART 协议（帧、CRC-8、重试），与 RTL 逐字节一致
vivado.py         常驻 Vivado Tcl 会话：program / XADC / JTAG-to-AXI；自动查找 Vivado 安装
sim_board.py      仿真板：同一协议的字节级模型 + 与 6571 仿真共享的虚拟芯片
regmap.json       寄存器表（插件、仿真、页面共用；与 rtl/dm_ctrl_regs.v 保持一致）
rtl/              平台 RTL（见下）
tools/verify_fpga.py   8881 上的验证工具（生成日志）
tests/            python -m unittest discover -s devices/fpga_k7/tests
```

## RTL（`rtl/`）

| 文件 | 内容 |
|---|---|
| `dm_uart.v` | 8N1 UART 收 / 发 |
| `dm_uart_bridge.v` | UART → 简单寄存器总线（读写、最多 255 字连续读、CRC、超时） |
| `dm_axil_adapter.v` | AXI4-Lite → 同一总线（接 Vivado 的 JTAG-to-AXI Master） |
| `dm_ctrl_regs.v` | 平台寄存器（地址表见文件头 / `regmap.json`） |
| `dm_platform_top.v` | 顶层示例：200 MHz 时钟、DUT 时钟分频/门控（BUFGCE）、复位、三态管脚、`chip_top` 实例 |
| `chip_top_stub.v` | `chip_top` 的端口约定（替换成你的芯片 RTL） |
| `dm_platform_k7_1v8.xdc` | 约束模板：LVCMOS18、与芯片一致的上下拉、SWCLK 时钟约束；`TODO` 处填板子管脚 |
| `sim/` | 仿真：`python rtl/sim/run_sim.py`（`pip install yowasp-yosys`，用 Yosys 检查综合 + CXXRTL 逐周期仿真，不需要 Vivado） |

**集成到你的工程：**
1. 把 `dm_uart.v` `dm_uart_bridge.v` `dm_ctrl_regs.v`（要用 JTAG 访问的话再加 `dm_axil_adapter.v`）加进工程。
2. 参照 `dm_platform_top.v`，把 `chip_top` 换成你的芯片顶层，接上 trim 寄存器输出和想观察的内部信号（`probes`）。
3. 在约束模板里填好 UART、SWD、GPIO、LED 的管脚，以及 Bank 0 的 CFGBVS。
4. 生成时把 `BUILD_ID` 参数设为构建时间（例如 `0x26102917`），上位机读出后就能确认板上跑的是哪个版本。

仿真结果（本仓库内）：UART 桥覆盖读、写、连续读、CRC 错误、非法命令、零长度读、总线超时、SWCLK 计数、DUT 控制；AXI 适配器覆盖握手和 SLVERR。两个 testbench 全部通过；人为改坏 CRC 多项式后，testbench 能报出失败。

## UART 协议

```
请求  A5 | CMD | SEQ | ADDR[4] LE | ARG | CRC8         CMD 01 读：ARG = 字数(1..255)    CMD 02 写：ARG = DATA[4] LE
响应  5A | SEQ | N | DATA[4N] LE | STATUS | CRC8       STATUS 0 OK · 1 CRC 错 · 2 非法命令 · 3 总线超时
CRC-8/SMBUS（多项式 0x07、初值 0），计算范围是同步字节之后的所有字节
```

## 方法

| 类别 | 方法 |
|---|---|
| JTAG | `jtag_connect` `list_bitstreams` `program(bitstream, probes)` `xadc` |
| 寄存器 | `ping` `status` `reg_dump` `reg_read(address\|名称, count)` `reg_write(address\|名称, value)` |
| 虚拟芯片 | `dut_reset(hold)`（释放即上电复位）`dut_clock(enable, mhz)` `set_mode(mode)` `swclk_edges` |
| 模拟量桩 | `analog_model`：VREF / VLDO / FOSC，模型参数可在工位里用 `options.analog` 覆盖 |

## 工位

* `stations/sim_fpga.json`：纯仿真。FPGA 和 6571 的仿真共享同一颗虚拟芯片（`shared_dut`）。
* `stations/bench_8881_fpga.json`：实机。需要填 COM 口、bitstream 目录和 6571 引脚映射。
* 序列 `sequences/fpga_cosim_bringup.py`：FPGA 链路检查 → DUT 复位与时钟 → 6571 到 FPGA 的 SWCLK 连线 → SWD → SWD 与 UART 交叉校验 trim → 基于模拟量桩的 trim 闭环 → 复位控制 → 内核时钟扫描，共 21 项。

## 上机待确认（`tools/verify_fpga.py` 逐项检查）

* Vivado 的启动方式（`vivado.bat -mode tcl` 是否能通过管道交互）以及安装路径的自动查找。
* 硬件对象属性名：`IDCODE_HEX`、`REGISTER.CONFIG_STATUS.BIT14_DONE_PIN`，以及 sysmon 的 `TEMPERATURE`/`VCCINT`/`VCCAUX`/`VCCBRAM`。
* JTAG-to-AXI 的 `DATA` 返回格式（目前每次只读 1 个字）。
* 板子 USB-UART 的 COM 口，以及能否稳定跑 921600 波特率（不行就同时改 RTL 参数 `BAUD` 和工位的 `baud`）。
