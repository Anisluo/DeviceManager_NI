# DeviceManager SDK

插件开发接口。宿主（DeviceManager）只通过这里定义的 C ABI 与插件通信，插件不需要写任何界面代码：界面由插件目录里的 `page.json` 描述，宿主动态渲染。

```
sdk/
  include/dm_plugin.h     C ABI（唯一的二进制契约）
  include/dm/plugin.hpp   C++ 便捷层：派生 dm::Plugin + DM_EXPORT_PLUGIN 宏
  include/dm/json.hpp     无依赖的小型 JSON（可换成 nlohmann/json 等）
  template/               C++ 插件模板（复制改名即可）
  template_python/        Python 插件模板
  python/dm/              Python 包：插件基类 dm.plugin + 脚本 API（dm.device / record / sleep …）
  python/tests/           Python SDK 单元测试（不需要宿主）
  tests/abi_smoke.cpp     不依赖 Qt 的 ABI 冒烟测试
```

## 1. 插件目录

```
devices/<id>/
  plugin.json     清单（宿主不加载 DLL 就能列出插件）
  page.json       页面描述
  README.md       插件说明（在宿主"插件"页显示）
  CMakeLists.txt  dm_add_plugin(<id> SOURCES src/xxx.cpp)
  src/...
```

### plugin.json

| 字段 | 说明 |
|---|---|
| `id` | 唯一 ID，同时是目录名和默认库名 |
| `name` / `version` / `description` | 显示用 |
| `type` | `cpp`（共享库）或 `python` |
| `entry` | cpp：库名（不含前缀/扩展名），默认 = `id`；python：`模块:类`，如 `sim_dmm:SimDmm` |
| `abi` | 必须为 `1` |
| `category` | `instrument`（仪器）/ `task`（测试任务，出现在侧栏"测试任务"）/ `tool` |
| `capabilities` | 能力标签，如 `dcpower.source_voltage`、`simulated` |
| `page` | 页面文件，默认 `page.json` |
| `default_config` | 工位文件未给出的配置项的默认值 |

## 2. C ABI（`dm_plugin.h`）

```c
int         plugin_abi_version(void);                 // 返回 DM_ABI_VERSION
const char* plugin_manifest(void);                    // 静态 JSON
void*       plugin_create(const char* config_json);   // 每个设备会话一个句柄
int         plugin_invoke(void* h, const char* method, const char* args_json,
                          char* out_json, int out_len);
void        plugin_destroy(void* h);
void        plugin_set_callback(void* h, dm_event_cb cb, void* user);
```

* `config_json` = 工位文件中该设备除 `name`/`plugin` 以外的全部字段，例如
  `{"resource":"PXI1Slot8","simulate":true,"options":{...}}`。
* `plugin_invoke` 返回 `>=0` 表示成功（值为写入长度）；负数为错误码，`out_json` 中带 `{"error":"..."}`。
* 结果超出缓冲区时返回 `DM_E_BUFFER` 并保留结果，宿主再调用 `"$fetch"` 取回，**不会重复执行方法**。
* `"$methods"` 返回方法列表 `[{name, doc, args}]`，宿主的"方法调试"面板用它。
* 宿主保证同一句柄上的调用串行（每台设备一个工作线程），不同设备可并行。
* 事件回调可在任意线程调用；约定类型：`{"type":"log","level","message"}`、`{"type":"state",...}`、`{"type":"data",...}`。

### C++ 便捷层

```cpp
#include <dm/plugin.hpp>

class MySmu : public dm::Plugin {
public:
    explicit MySmu(const dm::Json& cfg) {
        method("set_voltage", "Set voltage", {"channel", "voltage"}, [this](const dm::Json& a) {
            dm::require(a.contains("voltage"), "voltage required");   // -> DM_E_BAD_ARGS
            // ... 调仪器驱动 ...
            return dm::Json{{"ok", true}};
        });
    }
};
DM_EXPORT_PLUGIN(MySmu, R"({"id":"my_smu","name":"My SMU","version":"0.1.0"})")
```

异常会自动转换为错误码：`dm::PluginError(code, msg)` → 指定错误码；`dm::JsonError` → `DM_E_BAD_ARGS`；其他 `std::exception` → `DM_E_FAIL`。`emit_event()` / `log()` 发送异步事件。

### Python 插件

语义与 C++ 插件完全相同，宿主用内嵌的 CPython（pybind11）加载，不需要编译：

```python
from dm.plugin import Plugin, PluginError, method, E_HW

class SimDmm(Plugin):
    def __init__(self, config):
        super().__init__(config)            # self.config / resource / simulate / options
        self.nplc = 1.0

    @method("Configure NPLC")
    def configure(self, nplc: float = 1.0):
        if not 0.01 <= nplc <= 100:
            raise ValueError("nplc out of range")      # -> DM_E_BAD_ARGS
        self.nplc = nplc                               # 返回 None -> {"ok": true}

    @method("Take one reading")
    def read(self):
        return {"value": 1.234}
```

* JSON 参数按名字作为关键字参数传入；参数多了或少了都会在调用前被拒绝（`DM_E_BAD_ARGS`）。
* `PluginError(msg, code)` 指定错误码；`ValueError` → `DM_E_BAD_ARGS`；其他异常 → `DM_E_FAIL`（带异常类型）。
* 结果中的 NaN/inf 变成 `null`，numpy 标量/数组自动转换。
* `self.emit({...})` / `self.log(msg, level)` 发事件；在 `__init__` 里发的事件会缓存到宿主连上之后再送出。
* 插件里可以用 `dm.Device("smu1").invoke(...)` 调用工位里的其他设备（sim_dmm 的 probe 就是这样实现的）。
* 保存 `.py` 文件后宿主自动重新加载插件并重建设备会话（同目录的辅助模块也会被监视和重新加载）。
* 同目录的辅助模块（如 `swd.py`）要在入口文件的**模块顶层** import：宿主只在加载入口文件的那一刻把插件目录放进 `sys.path`，在 `__init__` 或方法里再 import 会失败。
* 每次调用都会获取 GIL；耗时操作（等待仪器）应放在会释放 GIL 的驱动调用里，nimi-python 本身就是这样。

## 3. 脚本 API（`import dm`）

脚本控制台、`sequences/*.py` 序列文件和 Python 插件都可以用：

```python
smu = dm.device("smu1")                                     # 不存在时抛 KeyError
smu.invoke("set_voltage", {"channel": 0, "voltage": 1.2})   # 显式形式
smu.set_output(channel=0, enabled=True)                     # 属性形式，等价
m = smu.measure(channel=0)                                  # 返回 dict；失败抛 dm.DeviceError
dm.record(vin=1.2, iin=m["current"])                        # 一行数据，进入"数据"页的表格和 XY 图
dm.progress(0.5, "VIN = 1.2 V")                             # 进度条
dm.sleep(0.1)                                               # 能被"停止"按钮打断
dm.log("message", "warning")                                # 写入应用日志
dm.sweep(0, 1, 0.1)    # 含端点的等步长序列，11 点，无浮点累计误差
dm.linspace(0, 1, 5)
```

序列文件和控制台里已预先导入 `dm, device, devices, record, sleep, log, progress, linspace, sweep, math, time`。

* 脚本在独立线程运行，界面不卡；同一时刻只运行一个脚本。
* **停止**：`dm.sleep()` 和设备调用会立即抛 `KeyboardInterrupt`；纯 Python 循环在 300 ms 后被异步中断。用 `try/finally` 保证关输出。
* 脚本跑完后，它的变量留在控制台命名空间里，可以接着交互分析。

## 3b. 扫描、Shmoo 与数据（`dm.scan` / `dm.data` / `dm.report`）

```python
from dm import Axis, Scan, shmoo
r = Scan("线性调整率",
         axes=[Axis("rload", [3300, 330], set=lambda v: smu.set_load(channel=0, r_load=v), unit="Ω"),
               Axis("vin", sweep(3.0, 5.5, 0.05), set=lambda v: smu.set_voltage(channel=0, voltage=v), settle=0.005, unit="V")],
         measure=lambda p: {"vout": dmm.read()["value"]},
         setup=..., teardown=..., snake=False).run()        # 每点一行，自动进度，可停止，teardown 一定执行
shmoo("VDD × SWCLK", x=Axis(...), y=Axis(...), test=lambda p: dig.swd_check()["pass"])
t = dm.data.load(r.run_id); t["vout"]; t.where(rload=330)    # 读回数据库
dm.report.export(r.run_id, fmt="pdf")                       # HTML / PDF 报告（matplotlib）
```

所有 `dm.record()` 都写入 SQLite（工位文件的 `data_dir`，默认 `<prefix>/data/devicemanager.sqlite`）。每个脚本一次运行；`dm.new_run(name, kind, **meta)` 可以在脚本里另起一次。左侧栏"测试数据"中可以浏览、作图，导出 CSV / HTML / PDF / Parquet（Parquet 需要 pyarrow）。

## 4. page.json

```jsonc
{
  "title": "仿真 SMU",
  "subtitle": "通道 0",
  "columns": 12,                        // 栅格列数
  "sources": [                          // 轮询数据源，控件按 id 订阅
    { "id": "ch0", "method": "measure", "args": { "channel": 0 }, "interval_ms": 200 }
  ],
  "sections": [                         // 卡片
    { "title": "输出设置", "span": 4, "row_span": 2, "layout": "column",   // layout: column | row
      "widgets": [ ... ] }
  ]
}
```

一个数据源在上一次调用返回前不会再次发出请求，所以慢仪器不会积压请求。窗口宽度不足 1000px 时，所有卡片自动占满整行。

### 控件

所有控件可带 `"id"`，宿主中名为 `widget:<id>`，统一提供 `apply(value)`（命令行 `--action <id>=<value>` 和 M2 的脚本会用到）。

| type | 字段 | 行为 |
|---|---|---|
| `number` | `label` `unit` `min` `max` `step` `decimals` `value` `init` `action` | 回车/失焦后提交；↑/↓ 按 step 增减；支持 SI 后缀输入（`10m`、`2.5k`、`1u`） |
| `combo` | `label` `options:[{label,value}]` `init` `action` | 选择后提交 |
| `switch` | `label` `value` `init` `action` | `$value` 为 true/false；调用失败时自动回退 |
| `button` | `label` `style`(primary/secondary/ghost) `action` `reload_page` | `reload_page` 为 true 时调用成功后重读所有 `init` |
| `readout` | `label` `source` `field` 或 `expr` `unit` `digits` `states` | 工程计数法显示；`expr` 例如 `"voltage * current"`；`states` 把值映射为状态标签 `{ "true": {text, tone} }` |
| `trend` | `height` `window_s` `series:[{source, field/expr, label, unit, axis, color}]` | 滚动趋势图，左右双轴，可暂停/清除/切换窗口，悬停读数 |
| `table` | `source` `rows_field` `columns:[{field,label,unit,digits}]` | 表格（数组字段或单个对象） |
| `log` | `height` | 该设备的事件和失败调用 |
| `script` | `file`（相对 sequences/）或 `code`，`height` `editable` | 嵌入序列脚本：编辑、保存、运行/停止；输出在底部脚本控制台 |
| `text` | `label` `value` `placeholder` `mono` `action` | 文本输入（路径、图形标签…） |
| `sweep` | `name` `kind`(scan/shmoo) `axes[]` `measure[]` `pass` `setup[]` `teardown[]` | 声明式扫描，不写 Python；轴范围可在页面上改；`"device": "$self"`（默认）指本页设备 |
| `xy` | `x` `y`（字符串或数组）`group` `x_unit` `y_unit` `height` | 当前/上一次扫描的 XY 曲线，按 `group` 分族，实时刷新 |
| `shmoo` | `x` `y` `value`（默认 `pass`）`x_unit` `y_unit` `height` | 当前/上一次扫描的通过/失败（或数值）热力图 |

**表单与结果**（多个输入配一个按钮的场景，例如"地址 + 字数 → 读取"）：

* `number` / `combo` / `text` 不写 `action` 时只保存值，按钮参数用 `"$form.<id>"` 引用；数据源的 `args` 里也可以用。
* 按钮加 `"publish": "<id>"` 后，返回结果会像轮询数据源一样发布出去，`readout` / `table` 用 `source` 绑定即可。
* `number` 支持 `"format": "hex"`，输入也接受 `0x…`；`readout` 的 `"format": "hex"` 同理。
* `combo` 可以用 `"options_from": { "method", "args", "field" }` 从插件方法动态获取选项。
* `table` 的列可以加 `"tones": { "fail": "error", "ok": "ok" }` 给单元格着色，加 `"format": "hex"` 显示十六进制。
* 页面可以分标签页：`{ "tabs": [{ "title", "sections": [...] }] }`，`sources` 由各标签页共享。

* `action`：`{ "method": "...", "args": { ..., "x": "$value" } }`，参数中的 `"$value"` 被替换为控件值。
* `init`：`{ "method", "args", "field" }`，页面打开、设备重连、按"重新加载页面"时读取初始值。
* `field` 支持点路径，例如 `channels.0.voltage`。
* 未知的控件类型显示占位卡片，不影响页面其他部分。

## 5. 工位文件（station.json）

```json
{
  "name": "仿真开发工位",
  "plugin_paths": ["../../more_devices"],
  "default_page": "smu1",
  "devices": [
    { "name": "smu1", "plugin": "sim_smu", "resource": "SIM1", "simulate": true, "options": { "channels": 4 } }
  ]
}
```

`plugin_paths` 相对工位文件；宿主还会搜索源码树 `devices/` 和安装目录 `<prefix>/devices`。

## 6. 热重载

宿主从影子副本（`bin/.shadow/`）加载插件库，原文件不被锁定。重新编译 C++ 插件或保存 Python 插件文件后，宿主自动关闭并重建使用它的设备会话；修改 `page.json` 后页面立即重新渲染。
