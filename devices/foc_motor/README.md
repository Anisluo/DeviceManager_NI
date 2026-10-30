# foc_motor — FOC 电机调试

C++ 插件（`foc_motor.dll`），通过 USB 串口和 PMSM（带编码器）FOC 控制板通信，协议为 **DMFOC**，固件端
C 代码在 `firmware/`。没有控制板时用内置仿真控制板（`port: "sim"`），它跑的是同一套固件协议代码。

## 页面

| 标签 | 内容 |
|---|---|
| 运行 | 状态/故障、模式与目标、启动/停止/急停/清故障、转速与电流趋势（100 ms 刷新）、端口选择与连接、仿真负载 |
| 参数 | 固件上报的完整参数表，按组显示；回车写入，固件限幅后的实际值会回显；保存到 Flash / 恢复默认 |
| 整定 | 电流/速度/位置环阶跃测试：上升时间、超调、调节时间、稳态误差 + 波形；下方直接改三环参数，改完再测 |
| 示波器 | 选通道（逗号分隔）、分频、点数、触发通道/电平/边沿；立即采集或等待触发。数据在固件 RAM 中以 FOC 中断速率采集，再整块读回，不受串口带宽限制 |
| 校准 / 辨识 | 编码器电角度零位和方向校准；Rs（两点直流法）、Ld（电压阶跃 di/dt）辨识，可一键写回 |

## 设备配置

```json
{ "name": "motor1", "plugin": "foc_motor", "simulate": false,
  "options": { "port": "COM3", "baud": 921600, "auto_connect": true } }
```

`simulate: true` 时默认端口为 `sim`。也可以在页面上随时换端口重连。示例站点：
`stations/sim_bench.json`（仿真）、`stations/bench_motor.json`（真实控制板，改 COM 口即可）。

## 方法（脚本可用）

```python
m = dm.device("motor1")
m.connect(port="COM3", baud=921600)
m.calibrate()                                  # 首次上电必须
m.param_write(name="spd_kp", value=0.012)
m.run(mode="SPEED", target=1500)
r = m.step_test(loop="speed", amplitude=500, base=1000)
dm.record(rise_ms=r["metrics"]["rise_ms"], overshoot=r["metrics"]["overshoot_pct"])
m.scope_config(channels="ia,ib,ic", divider=1, length=1000, trigger="ia", level=0, edge="rising")
cap = m.scope_capture(timeout=2)               # {t: [ms], channels: [{name, unit, data}]}
m.command(name="disable")
```

全部方法：`list_ports connect disconnect get_info status params param_read param_write command set_mode
set_target run scope_config scope_capture scope_last step_test calibrate identify sim_set_load sim_set_vbus`。

## 固件集成

1. 把 `firmware/dmfoc_proto.h`、`dmfoc_proto.c` 加进工程（C99，无 malloc，约 3 KB RAM + 示波器缓冲）。
2. 参照 `firmware/dmfoc_port_example.c` 写参数表、通道表和三个回调（`tx`、`command`、`state`）。
3. 主循环里把串口收到的字节交给 `dmfoc_rx()`；FOC 中断末尾调用 `dmfoc_scope_sample()`。

上位机连接时读取参数表和通道表，所以固件里加一行参数，页面上就多一行，不用改插件。
页面依赖的参数名：`enc_offset enc_dir calibrated`（校准）、`rs ld lq rs_est ld_est`（辨识）、`accel`（速度阶跃测试时临时调大）。

## DMFOC 协议

```
A5 5A | TYPE | SEQ | LEN(u16 LE) | PAYLOAD | CRC16(u16 LE)
CRC-16/CCITT-FALSE（poly 0x1021，init 0xFFFF），覆盖 TYPE..PAYLOAD
应答 TYPE = 请求 | 0x80，SEQ 相同；出错 TYPE = 0xFF，payload = {请求类型, 错误码}
```

| 类型 | 名称 | 请求 → 应答 |
|---|---|---|
| 01 | PING | → 版本、参数数、通道数、示波器容量、采样率、固件名 |
| 02 | PARAM_INFO | id → 类型、标志、范围、名称/单位/分组/枚举 |
| 03 | PARAM_READ | 起始 id、数量 → 值 |
| 04 | PARAM_WRITE | id、值 → 状态、实际写入值（可能被限幅） |
| 05 | CHANNEL_INFO | id → 名称、单位 |
| 06 | COMMAND | 命令、参数 f32 → 结果 |
| 07 | STATUS | → 状态、模式、故障位、全部通道当前值 |
| 08–0B | SCOPE_CONFIG / ARM / STATUS / READ | 配置、触发、查询、分块读回 |

命令：1 使能、2 停止、3 清故障、4 设模式、5 设目标、6 校准、7 辨识、8 保存、9 恢复默认、10 急停。
状态：IDLE / RUN / FAULT / CALIBRATE / IDENTIFY；模式：IDLE / VF / CURRENT / SPEED / POSITION。
故障位：0 过流、1 过压、2 欠压、3 过温、4 编码器；位 16 = 未校准（提示，不算故障）。
字段的精确定义见 `dmfoc_proto.h`。

## 仿真控制板

20 kHz 实时线程：PMSM dq 模型（7 对极，Rs 0.35 Ω，Ld 0.40 mH，Lq 0.45 mH）、14 位编码器（安装偏差随机，
上电未校准）、电流 PI + 解耦、速度 PI、位置 P、VF 开环、过流/过压/欠压/过温保护和热模型。
负载转矩和母线电压可在页面或脚本里改（`sim_set_load`、`sim_set_vbus`）。

## 测试

`ctest -R foc_motor_closed_loop`：经 C ABI 走完连接、校准、速度/电流/位置阶跃、辨识、示波器触发。
