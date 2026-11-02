# 通信协议仿真

与 TDC 测量一样作为 `category: task` 插件自动出现在侧栏；纯 Python，不依赖任何仪器、NI 驱动或第三方 Python 包。

每种协议一个标签页。修改参数后点“仿真”：共享时基的数字通道、字段解码条及字段记录一起更新。
滚轮缩放，左键框选时间，右键拖动平移，双击显示全部；顶部标尺左右键设置 C1/C2，拖动游标测量时间差和频率。
字段悬停可查看详情；错误字段标红。页面首次进入时自动生成默认示例，各协议分别保留最近一次结果。
SPI/UART/I²C/SWD 的串行数据带有逐位标记：例如 `D7=1` 表示数据第 7 位为 1；
虚线分隔位区间，三角形表示采样时刻。SPI 位号随 MSB/LSB 设置变化，I²C 地址显示 A6…A0 / R/W，SWD 请求显示各控制位名称。
缩小时自动隐藏过密的位文字；放大或悬停可查看完整位名、字节/字段和采样时间。
时钟上仅在协议配置使用的有效采样沿显示小三角，不显示边沿文字或另一种边沿的标记。SPI 随 CPOL/CPHA 配置选择上升/下降采样沿；悬停可查看采样时刻。

| 协议 | 范围 |
|---|---|
| SWD | DP/AP 单次读写、线路复位、8 位请求、周转、OK/WAIT/FAULT、32 位数据、偶校验；HOST_OE 显示主机是否驱动 |
| SPI | 模式 0–3、MSB/LSB、CS#、SCLK、MOSI/MISO，1–64 字节全双工 |
| I²C | 7 位地址、单次读写、START/STOP、ACK/NACK、时钟延展；NACK 序号 -1 为关闭，0 为地址，1 起为数据 |
| PCIe | 32 位事务接口节拍示意：3DW MRd/MWr 请求、CplD 响应、可设完成延迟，4–64 字节负载 |
| AXI | AXI4，32 位、单 ID、INCR burst、1–16 拍，读/写通道、VALID/READY、背压、LAST、OKAY/SLVERR/DECERR |
| UART | 5–8 数据位、无/奇/偶校验、1/2 停止位、LSB 优先，首帧校验或停止位错误 |
| CAN | Classic CAN 2.0A，11 位标准 ID、1–8 字节数据帧、CRC-15、位填充、ACK、CRC/ACK 错误场景 |

这是确定性事务时序模型。解码字段来自生成事务；不提供外部采样信号的自动解码，也不驱动真实硬件。
SWD 从已处于 SWD 模式开始，不含 JTAG 切换或 AP posted-read 管线；I²C 不含 repeated START 或多主机仲裁。
CAN 不含扩展帧、远程帧、CAN FD、多节点仲裁、错误计数/重传或差分模拟电压。
AXI 地址/数据/响应顺序执行，不含多 ID 并发或乱序；地址限制为 32 位且 burst 不跨 4 KiB。
PCIe 页明确是事务接口示意，不是串行差分波形：未实现 LTSSM、链路训练、DLLP、流控、LCRC、8b/10b 或 128b/130b 编码。

## 导出和脚本

每页可导出最后一次仿真为 VCD（1 ps 时标）或 JSON。目标文件须不存在；VCD 包含数字线及 AXI 数值总线，
PCIe 的包字段只保存在 JSON 中，不伪造串行信号。JSON 同时保留参数、波形和字段记录。

```python
from dm import device
cap = device("protocol_sim").simulate(protocol="SPI", frequency=1e6,
    options={"mode": 0, "data": "9F 00 00 00", "response": "00 EF 40 18"})
```

插件 ABI 方法：`simulate(protocol, frequency, options)`、`capture(protocol)`、`export(protocol, path, format)`。
Python 类内部方法名为 `generate`，避免与基类的 `simulate` 布尔属性冲突。

页面生成：`python devices/protocol_sim/tools/generate_page.py`。
测试：`python -m unittest discover -s devices/protocol_sim/tests -v`；CMake/CTest 中对应 `protocol_sim`。

模型参考：[NXP I²C UM10204](https://www.nxp.com/docs/en/user-guide/UM10204.pdf)、
[Arm AXI](https://developer.arm.com/community/arm-community-blogs/b/soc-design-and-simulation-blog/posts/introduction-to-axi-protocol-understanding-the-axi-interface)、
[Arm CMSIS-DAP SWD 实现](https://github.com/ARMmbed/DAPLink/blob/main/source/daplink/cmsis-dap/SW_DP.c)、
[NI SPI 模式说明](https://www.ni.com/en/support/documentation/supplemental/08/using-spi-with-labview-and-the-usb-8451.html)、
[CiA CAN CRC](https://can-cia.org/can-knowledge/cyclic-redundancy-check-crc-in-can-frames)。
