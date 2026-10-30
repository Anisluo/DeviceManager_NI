# sim_dmm — 仿真 DMM（Python 插件）

用 Python 写的插件示例，接口语义与 C++ 插件相同（`plugin.json` 中 `"type": "python"`，`"entry": "sim_dmm:SimDmm"`）。保存 `sim_dmm.py` 后宿主会自动重新加载。

## 模型

* 通过 `options.probe` 读取工位里另一台设备的某个节点，例如 sim_smu 的 LDO 输出：
  `{"device": "smu1", "method": "measure", "args": {"channel": 0}, "field": "load_v"}`。
  不配 probe 时读 `set_manual_value` 设置的值。
* 误差模型：增益误差 `gain_ppm`（默认 12 ppm）、失调 `offset_uv`（默认 3 µV）、噪声 `noise_ppm_range`（默认 1 NPLC 时为量程的 0.3 ppm rms，按 1/√NPLC 下降）。
* 每次读数耗时 NPLC / 50 Hz。

## 方法

| 方法 | 参数 | 说明 |
|---|---|---|
| `configure` | `function`（dcv/dci） `range` `nplc` | 自动选能容纳该值的最小量程 |
| `get_config` | — | 当前配置 |
| `set_manual_value` | `value` | 没有 probe 时的读数 |
| `read` | — | `value` `overload` `range` `nplc` `n` |
| `read_stats` | `count` | `mean` `std` `min` `max` |
