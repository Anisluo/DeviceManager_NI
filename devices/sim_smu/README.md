# sim_smu — 仿真 SMU

4 通道仿真源测量单元，用于无硬件时开发页面、测试序列和宿主本身。

## 模型

* 电压源 ±6 V，限流 (0, 0.5] A；超过限流时进入**限流状态**：电流钳位在限值，端电压按负载模型回落。
* 负载模型（`set_load`）：
  * `resistor`：I = V / R
  * `diode`：Shockley 模型，Is = 1 pA，n = 1.5
  * `ldo`：Vout = min(Vset, Vin − 压差)，I = Vout / R_load + Iq；压差区 Iq 增大到 2.5 倍
* 测量值叠加高斯噪声（`options.noise_v` 默认 20 µV，`options.noise_i` 默认 2 nA）。

## 配置（station.json 中的设备字段）

```json
{ "name": "smu1", "plugin": "sim_smu", "resource": "SIM1",
  "options": { "channels": 4, "noise_v": 2e-5, "noise_i": 2e-9,
               "power_on": { "voltage": 3.6, "current_limit": 0.05, "output": true, "load": "ldo", "r_load": 330 } } }
```

## 方法

| 方法 | 参数 | 返回 |
|---|---|---|
| `get_info` | — | `resource` `model` `channels` `simulated` |
| `reset` | — | 全部通道 0 V、输出关 |
| `set_voltage` | `channel` `voltage` | `{ok}` |
| `set_current_limit` | `channel` `current_limit` | `{ok}` |
| `set_output` | `channel` `enabled` | `{ok}`，并发出 `state` 事件 |
| `set_load` | `channel` `load` `r_load` `ldo_vout` `ldo_iq` | `{ok}` |
| `get_config` | `channel` | 通道设置 |
| `measure` | `channel` | `voltage` `current` `load_v`（负载输出节点电压，LDO 模型为稳压输出；无噪声）`compliance` `output` `t` |
| `measure_all` | — | `{channels:[...]}` |

进入/退出限流时发出 `{"type":"state","compliance":true}` 事件和一条 warning 日志。
