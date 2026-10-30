# DeviceManager — 插件化芯片测控上位机

宿主程序本身不含测试逻辑：按工位文件（station.json）加载 `devices/` 下的插件，按插件的 `page.json` 动态生成测控页面。

> 注：仓库根目录的 `README.md` / `server.cpp` 等属于另一个独立的小工具，与本项目无关。

## 目录

```
DeviceManager/   宿主（C++17 + Qt 6 / QML）
  src/core/      PluginLibrary（加载 + 影子副本）、PluginRegistry（发现 + 文件监视）、
                 Device（设备会话，每台设备一个工作线程）、Station、LogModel
  src/app/       AppController（QML 单例 App）、Python 语法高亮
  src/python/    内嵌 CPython：PythonRuntime（解释器 + _dmhost 模块）、PythonPluginModule、
                 ScriptEngine（脚本线程、停止、数据记录）
  qml/           主界面、侧栏、底部日志 / 脚本控制台、页面渲染器 qml/page/*、主题 Theme.qml
  stations/      工位文件
  sequences/     测试序列脚本（Python）
devices/         插件（每个子目录一个）
sdk/             C ABI 头文件、C++ 便捷层、模板、ABI 测试 —— 详见 sdk/README.md
scripts/         构建脚本
```

## 构建（Windows）

依赖：

| 依赖 | 位置 |
|---|---|
| Qt 6.8.3（mingw_64） | `D:\Qt\6.8.3\mingw_64` |
| MinGW 13.1（与 Qt 配套） | `D:\Qt\Tools\mingw1310_64` |
| pybind11 3.x（头文件 + CMake 配置） | `D:\Qt\_deps\pybind11` |
| Python 3.13（内嵌：标准库、site-packages、python313.dll） | `C:\Users\Admin\AppData\Local\Programs\Python\Python313` |

Qt 官方 MinGW 版基于 msvcrt，不能用 `D:\tools\mingw64` 的 GCC 16/UCRT 链接宿主；插件只走 C ABI，不受此限制。

宿主运行时把编译时找到的 Python 安装作为 PYTHONHOME（标准库和 numpy 等第三方包都从那里加载），可用环境变量 `DM_PYTHONHOME` 改指别的 Python 3.13。Python 不可用时宿主照常启动，只是 Python 插件和脚本被禁用（日志里说明原因）。

在 Git Bash 中：

```bash
DEPLOY=1 bash scripts/build.sh
```

产物：`build/bin/DeviceManager.exe`（`DEPLOY=1` 会把 Qt 运行库拷进 `build/bin`），插件在 `build/devices/<id>/`。
Linux：`cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=<Qt6>` 后 `cmake --build build`。

## 运行

```bash
build/bin/DeviceManager.exe --station DeviceManager/stations/sim_bench.json
```

| 选项 | 说明 |
|---|---|
| `--station <file>` | 工位文件（默认上次使用的，或 `stations/sim_bench.json`） |
| `--view device:smu1` / `plugin:sim_smu` / `station` | 启动页面 |
| `--theme light\|dark` | 主题（界面右上角也可切换，会记住） |
| `--action <id>=<value>[@ms]` | 像用户输入一样驱动带 id 的控件，可重复；`console=data` / `output` / `stop` / `exec:<python>` 操作脚本控制台 |
| `--run <file>[@ms]` | 启动后运行 `sequences/` 下的序列脚本 |
| `--console log` / `script` | 启动时展开底部面板 |
| `--screenshot <png>` | 截图并退出，同时写出 `<png>.log`（CI / 回归用） |
| `--native-frame` | 使用系统标题栏（默认在 Windows 上绘制与主题一致的标题栏；也可设环境变量 `DM_NATIVE_FRAME=1`） |

不需要人工操作的端到端检查：

```bash
build/bin/DeviceManager.exe --view device:smu2 --action vset=2.5@1500 --action output=true@2500 --action ilim=1m@4000 --screenshot check.png --screenshot-delay 6000
```

```bash
build/bin/DeviceManager.exe --run ldo_line_regulation.py@1500 --action console=data@4500 --screenshot sweep.png --screenshot-delay 6000
```

第一条：页面控件 → C++ 插件。第二条：Python 序列 → C++ SMU + Python DMM，结果在"数据"页。

## 脚本控制台

底部面板"脚本控制台"：左边是序列编辑器（Ctrl+Enter / F5 运行，Ctrl+S 保存），右边是输出、`>>>` 交互提示符（↑/↓ 历史）和"数据"页（`dm.record()` 的表格 + XY 图）。脚本 API 见 `sdk/README.md` 第 3 节。

## 开发流程

* 修改 `devices/<id>/page.json` → 页面立即刷新（开发时直接读源码树）。
* 重新编译 C++ 插件 / 保存 Python 插件 `.py` → 宿主自动重建该插件的设备会话。
* 新插件：复制 `sdk/template`（C++）或 `sdk/template_python`（Python），见 `sdk/README.md`。
* 测试：`ctest --test-dir build` 运行 ABI 冒烟测试和 Python SDK 单元测试。
