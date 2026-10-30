# DeviceManager_NI

模拟芯片特性测试平台：基于 NI PXIe-1095（PXIe-8881 控制器）的插件式测控上位机。

| 目录 | 内容 |
|---|---|
| `DeviceManager/` | 主程序（C++17 + Qt 6 QML），工位配置 `stations/`、测试序列 `sequences/` |
| `devices/` | 设备插件，每个插件一个目录（`plugin.json` + `page.json` + README） |
| `sdk/` | 插件 C ABI 头文件、Python `dm` 包、C++ / Python 插件模板 |
| `scripts/` | `build.sh` 构建 + 测试，`package.sh` 打包 Windows 发布版 |

## 插件

| 插件 | 类型 | 说明 |
|---|---|---|
| `ni_digital` | Python | PXIe-6571 数字图形：pattern、SWD、shmoo、PPMU |
| `fpga_k7` | Python | XC7K480T 虚拟芯片平台：JTAG 下载 + UART 寄存器桥，附 RTL |
| `foc_motor` | C++ | FOC 电机调试：USB 串口 DMFOC 协议（附固件 C 代码）、在线整定、示波器、校准/辨识 |
| `sim_smu` / `sim_dmm` | C++ / Python | 仿真源表 / 万用表 |

所有真实设备插件都带仿真模式，没有硬件也能跑通整个流程。

## 构建

依赖：Qt 6.8（mingw_64）、MinGW 13.1、CMake ≥ 3.21、Python 3.13（嵌入）、pybind11。

```bash
bash scripts/build.sh          # 构建并运行 ctest
bash scripts/package.sh        # 生成 release/DeviceManager-<ver>-win64.zip
```

运行：

```bash
build/bin/DeviceManager.exe --station DeviceManager/stations/sim_bench.json
```

部署到 8881 见 [DeviceManager/DEPLOY_8881.md](../DeviceManager/DEPLOY_8881.md)，主程序说明见
[DeviceManager/README.md](../DeviceManager/README.md)，插件开发见 [sdk/README.md](../sdk/README.md)。
