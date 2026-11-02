# MCU助手

纯 Python 任务插件，无额外 Python 依赖。导入 CMSIS-Pack `.pack`（ZIP）或已解包 `.pdsc`，查看器件、内存、封装和烧录算法，解析 CMSIS-SVD，并编辑/导出离线寄存器初始化方案。

## 页面

1. **Pack与器件**：输入完整路径导入；按型号筛选，再打开器件/variant。显示内核、封装引脚数、GPIO 数、内存、功能。多核器件可选择处理器。
2. **烧录算法**：显示当前器件继承后的 FLM 文件、Flash 起始和大小、默认算法及文件是否存在。点击解析可查看 ELF32 ARM 符号、FlashDevice、页大小、扇区配置；可原样导出算法。
3. **引脚功能**：读取厂商包内提供的 MCU XML/CSV，或补充外部文件。CSV 需要 `number,name,functions` 列，可选 `type`；XML 支持 STM32 MCU 风格的 `Pin Position/Name/Type` 与 `Signal Name`。选择与器件及封装匹配的文件。
4. **寄存器配置**：选择外设，查看寄存器地址、权限、复位值和位域；编辑普通可写寄存器/位域的**离线值**。选中寄存器行后使用“查看位域”；可还原复位配置。
5. **配置导出**：仅导出明确编辑过的寄存器，支持 JSON 或 C 初始化方案，目标文件须不存在。

“载入教学示例”提供虚拟 DemoMCU8、8 条引脚信息和 GPIO 寄存器。示例故意不包含可执行的 FLM，算法文件状态显示不存在。

## 数据边界

- `.pack` 并不保证包含管脚编号、复用功能与封装图。**封装引脚数**可由 PDSC `feature` 中的 QFP/QFN/BGA 等条目获得；`IOs` 的 GPIO 数量不等于物理管脚数量。缺失数据不依据型号命名猜测。
- 包内候选引脚文件由路径筛选，选择它不代表已经自动验证适配具体封装；厂商没有提供时，需要外部 CSV/XML。PDF 数据手册不自动 OCR 或提取为引脚表。
- PDSC 解析 family→subFamily→device→variant 的属性及 processor/debug/memory/algorithm/feature/book 继承和覆盖。
- SVD 支持常用默认属性、peripheral/register/field/enumeratedValues 的 `derivedFrom`、cluster、dim/dimIncrement/dimIndex 数组、bitOffset/bitWidth、lsb/msb、bitRange、访问属性、复位值、枚举及数值范围约束。
  数组支持数值/字母范围和逗号索引；不支持的继承引用、非 8 位地址单位、无效位域或超过 64 位的寄存器会明确报告 SVD 解析失败，仍可浏览 Pack 基本信息。
- 配置值基于 SVD 复位值和用户离线编辑，**不是实时读取 MCU 的值**。缺省复位值用 0 建模；未编辑的寄存器不会导出。
- 只读项禁止编辑；字段存在时，未描述的保留位和只读位域不能修改；带 `modifiedWriteValues`、`readAction` 等特殊语义的寄存器仅供浏览。支持字段数值范围约束，未实现所有厂商扩展约束。
- C 导出支持 8/16/32/64 位寄存器：普通读写按配置掩码读改写，只写寄存器直接写值。代码是待审阅的初始化方案，不是芯片完整初始化流程，不自动配置时钟/复用关系或执行到设备。
- FLM 仅静态读取 ELF32 little-endian ARM 文件。符号值可能是链接地址或节内偏移，不能直接当作运行时入口；ELF 重定位、RAM 分配、SWD/JTAG 通信及真实烧录不在本版范围内。
- 导入时不解压、不执行包内代码，不修改厂商文件。包内路径不能逃出包根目录；XML 不支持 DTD/实体声明；读取的单文件限制 32 MiB。

## 源码与验证

- `mcu_assistant.py`：插件 API、引脚导入、配置及导出。
- `pack_model.py`：Pack/PDSC 与 FLM 读取。
- `svd_model.py`：SVD 寄存器模型。
- `page.json`、`tools/generate_page.py`：页面配置与生成脚本。
- `tests/test_mcu_assistant.py`：合成标准样例、继承/数组/权限、配置导出及真实 Pack 测试。

```powershell
python devices/mcu_assistant/tools/generate_page.py
python -m unittest discover -s devices/mcu_assistant/tests -v
ctest --test-dir build -R '^mcu_assistant$' --output-on-failure
```

真实样例：本机 `F:/66-TDC/Keil.STM32L0xx_DFP.2.2.0.pack`，识别 166 个器件/变体；STM32L010C6Tx 为 QFP48、Cortex-M0+，解析出 329 个寄存器，并正确读取默认 Flash FLM。
另验证 STM32L072CBTx 的 GPIOC 从 GPIOB 派生时地址正确变化。该厂商 Pack 不含逐封装引脚功能表，因此显示缺失而不是推断。
测试可通过 `MCU_ASSISTANT_TEST_PACK` 指定该真实样例；样例不在本仓库分发，其他机器不存在时跳过真实包检查。

格式参考：[Open-CMSIS-Pack 器件及继承规则](https://open-cmsis-pack.github.io/Open-CMSIS-Pack-Spec/main/html/pdsc_devices_pg.html)、
[CMSIS-SVD 寄存器及 derivedFrom](https://arm-software.github.io/CMSIS_5/SVD/html/elem_registers.html)。
