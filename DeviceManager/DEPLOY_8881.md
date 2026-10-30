# 在 PXIe-8881 上部署与验证 6571

## 1. 准备 8881（一次性）

1. **NI 软件**（NI Package Manager）：NI-Digital Pattern Driver，以及它附带的 Digital Pattern Compiler。用 NI MAX 确认 6571 在 `PXI1Slot15`；如果槽位不同，后面把 `PXI1Slot15` 换成实际的资源名。
2. **Python 3.13 64 位**（python.org 安装包）。宿主内嵌的是 3.13，版本必须一致；安装时勾选 "Add to PATH"。
3. 安装 Python 包：

   ```
   py -3.13 -m pip install nidigital hightime numpy matplotlib
   ```

   `matplotlib` 只用于导出 PDF/HTML 报告。

## 2. 解压

把 `DeviceManager-<版本>-win64.zip` 解压到任意目录，例如 `D:\DeviceManager`：

```
DeviceManager\
  bin\DeviceManager.exe        宿主（Qt 运行库已在 bin 里）
  devices\ni_digital\          6571 插件
  devices\ni_digital\tools\verify_6571.py
  stations\bench_8881_drvsim.json   驱动仿真工位
  stations\bench_8881.json          实机工位
  sequences\  sdk\python\  data\
```

宿主会自动从注册表找到 Python 3.13。如果找不到，日志里会说明原因，可以设置环境变量 `DM_PYTHONHOME=<Python 安装目录>`。

## 3. 验证（按顺序，每一步把生成的 .log 发回）

在 `devices\ni_digital\tools` 目录打开命令行：

**第 1 步：驱动仿真，不碰硬件，检查 API 调用。**

```
py -3.13 verify_6571.py --simulate
```

**第 2 步：实机，6571 不接 DUT（或 DUT 不上电），只检查仪器本身。**

```
py -3.13 verify_6571.py --resource PXI1Slot15 --no-dut
```

**第 3 步：接负载板并给 DUT 上电，按实际接线填写 `--pins`。**

```
py -3.13 verify_6571.py --resource PXI1Slot15 --vdd 3.3 --pins SWCLK=0,SWDIO=1,nRST=2,PA0=4,PA1=5,PA2=6,PA3=7
```

每次运行会在当前目录生成 `verify_6571_<时间>.log`，里面有软件版本、每一步的结果、原始 SWD 位、编译器命令行和输出、完整的异常堆栈。把整个文件发回即可。

> 安全提示：第 2、3 步中 PPMU 会在每个引脚上依次施加 0.02–0.22 V，开短路测试会灌 -100 µA，漏电测试会加 0 V 和 VDD。请确认负载板上没有不能承受这些条件的器件。

## 4. 打开界面

```
bin\DeviceManager.exe --station ..\stations\bench_8881_drvsim.json
bin\DeviceManager.exe --station ..\stations\bench_8881.json
```

确认左侧 dig1 为绿色，"概览"页的后端显示"NI-Digital 驱动"。底部"日志"里有任何红字，请截图发回。

## 5. XC7K480T 虚拟芯片平台

1. 在 8881 上安装 Vivado 或 Vivado Lab（2025.1 的统一安装包可以只选 Lab Edition），确认 hw_server 能看到 FPGA 的 JTAG 线。然后执行：

   ```
   py -3.13 -m pip install pyserial
   ```

2. 用 `devices\fpga_k7\rtl\` 里的平台 RTL 生成 bitstream（集成步骤见 `devices\fpga_k7\README.md`），把 .bit 放进 `bench_8881_fpga.json` 的 `bitstream_dir`。
3. 在 `devices\fpga_k7\tools` 目录下运行验证工具，日志同样发回：

   ```
   py -3.13 verify_fpga.py --list-ports
   py -3.13 verify_fpga.py --port COM3 --jtag --bitstream D:\fpga\bitstreams\platform.bit
   ```

4. 打开界面：`bin\DeviceManager.exe --station ..\stations\bench_8881_fpga.json`，然后运行序列 `fpga_cosim_bringup.py`。

## 已知待确认项（验证工具会逐项检查）

| 项 | 工具中的步骤 | 失败时 |
|---|---|---|
| PPMU 读数顺序是否等于引脚顺序 | 4 | 日志里有逐脚的施加值与读数，据此修正映射 |
| 图形编译器命令行 | 6 | 会自动尝试 5 种写法；全部失败时日志里有每次的输出 |
| 捕获向量的引脚状态（X / V） | 10 | 自动两种都试，并记录哪种可用 |
| SWDIO 选通位置、SWCLK 时序 | 10、12 | 日志里有原始 ACK 位和数据位 |
