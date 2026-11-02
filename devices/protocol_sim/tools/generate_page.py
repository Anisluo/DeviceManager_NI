"""Build the seven protocol tabs from shared controls and waveform sections."""
import json
from pathlib import Path


def number(key, label, value, low, high, unit=""):
    return {"type": "number", "id": key, "label": label, "value": value,
            "min": low, "max": high, "unit": unit, "decimals": 0}


def text(key, label, value):
    return {"type": "text", "id": key, "label": label, "value": value, "mono": True}


def combo(key, label, values, default=None):
    return {"type": "combo", "id": key, "label": label, "options": values,
            "value": values[0] if default is None else default}


CONFIG = {
    "SWD": (1000000, "请求 / 响应 / 周转 / 校验", [
        combo("operation", "操作", ["read", "write"]), combo("ap", "APnDP", [0, 1]),
        combo("address", "寄存器地址", [0, 4, 8, 12]), text("value", "32 位数据", "0x2BA01477"),
        combo("ack", "ACK", ["OK", "WAIT", "FAULT"]), combo("line_reset", "线路复位", [1, 0]),
        combo("parity_error", "校验错误", [0, 1])]),
    "SPI": (1000000, "全双工 · 四种 CPOL/CPHA 模式", [
        combo("mode", "SPI 模式", [0, 1, 2, 3]), combo("order", "位序", ["MSB", "LSB"]),
        text("data", "MOSI 数据", "9F 00 00 00"), text("response", "MISO 响应", "00 EF 40 18")]),
    "I2C": (100000, "7 位地址 · ACK/NACK · 时钟延展", [
        combo("operation", "操作", ["write", "read"]), text("address", "7 位地址", "0x50"),
        text("data", "数据", "00 A5 5A"), number("nack_at", "NACK 序号", -1, -1, 64),
        number("stretch", "延展周期", 0, 0, 100)]),
    "PCIE": (125000000, "TLP 事务接口示意 · 非串行物理层", [
        combo("operation", "请求类型", ["MRd", "MWr"]), text("address", "32 位地址", "0x1000"),
        text("data", "负载/读回", "11 22 33 44"), number("latency", "完成延迟", 8, 1, 100)]),
    "AXI": (50000000, "AXI4 · 32 位 INCR burst · VALID/READY 背压", [
        combo("operation", "操作", ["write", "read"]), text("address", "起始地址", "0x40000000"),
        text("data", "32 位字", "12345678 89ABCDEF"), number("stall", "背压周期", 2, 0, 16),
        combo("response", "响应", ["OKAY", "SLVERR", "DECERR"])]),
    "UART": (115200, "异步串口 · 起始位 / 数据 / 校验 / 停止位", [
        text("data", "发送字节", "48 65 6C 6C 6F"), combo("data_bits", "数据位", [8, 7, 6, 5]),
        combo("parity", "校验", ["none", "even", "odd"]), combo("stop_bits", "停止位", [1, 2]),
        combo("fault", "故障注入", ["none", "parity", "stop"])]),
    "CAN": (500000, "Classic CAN 2.0A · CRC-15 / 位填充 / ACK", [
        text("identifier", "11 位 ID", "0x123"), text("data", "数据 1–8B", "11 22 33 44"),
        combo("ack", "接收方 ACK", [1, 0]), combo("crc_error", "CRC 错误", [0, 1])]),
}

SCOPE = {
    "SWD": "单次 DP/AP 传输；从 SWD 模式开始，不含 JTAG 切换及 AP posted-read 管线。HOST_OE 区分主机驱动/释放，TURN 显示为虚线。",
    "SPI": "四种 CPOL/CPHA 模式，MSB/LSB 位序，发送与响应须等长。",
    "I2C": "单主机、7 位地址。NACK 序号 -1=不注入，0=地址，1 起为数据。支持时钟延展，不含多主机仲裁。",
    "PCIE": "TLP 事务接口，USER_CLK 按 32 位流节拍示意。支持 MRd / MWr 和 CplD；不含 LTSSM、DLLP、串行编码、LCRC 或物理差分信号。",
    "AXI": "AXI4 单 ID、32 位 INCR burst，1–16 拍；地址、数据、响应顺序执行，可设背压；不含并发及乱序。",
    "UART": "空闲高、LSB 优先，5–8 数据位，奇偶校验，1/2 停止位；故障注入只影响首帧。",
    "CAN": "Classic CAN 2.0A，11 位 ID、1–8 字节数据帧、CRC-15、位填充与 ACK；不含 CAN FD、多节点仲裁或差分电压。",
}


def main():
    page = {"title": "通信协议仿真", "subtitle": "逻辑分析仪式时序 · 字段解码 · 故障场景 · VCD / JSON 导出 · 无需硬件",
            "columns": 12, "sources": [], "tabs": []}
    for protocol, (rate, subtitle, controls) in CONFIG.items():
        prefix = protocol.lower()
        source = prefix + "_capture"
        page["sources"].append({"id": source, "method": "capture", "args": {"protocol": protocol}, "interval_ms": 2000})
        args = {}
        for control in controls:
            key = control["id"]
            control["id"] = prefix + "_" + key
            args[key] = "$form." + control["id"]
        controls.insert(0, number(prefix+"_frequency", "波特率" if protocol in ("UART", "CAN") else "时钟频率", rate, 1, 1000000000, "Hz"))
        controls.append({"type": "button", "id": prefix+"_simulate", "label": "仿真", "style": "primary", "fit": True,
                         "action": {"method": "simulate", "args": {"protocol": protocol, "frequency": "$form."+prefix+"_frequency", "options": args}}, "publish": source})
        info = [{"type": "readout", "source": source, "field": field, "label": label, **extra}
                for field, label, extra in [("duration", "总时长", {"unit": "s"}), ("event_count", "解码字段", {}),
                                            ("error_count", "异常字段", {}), ("description", "模型范围", {})]]
        page["tabs"].append({"title": "PCIe · TLP" if protocol == "PCIE" else "I²C" if protocol == "I2C" else protocol, "sections": [
            {"title": protocol+" 参数", "subtitle": subtitle + "。字节按十六进制输入，地址用 0x 前缀。修改参数后点“仿真”。", "span": 5, "widgets": controls},
            {"title": "仿真信息", "subtitle": SCOPE[protocol], "span": 7, "widgets": info},
            {"title": "时序与协议解码", "span": 12, "subtitle": "滚轮缩放，左键框选，右键拖动平移，双击显示全部。顶部标尺左/右键放置 C1/C2，悬停字段查看详情。", "widgets": [
                {"type": "logic", "source": source, "legend": "小三角仅标记当前协议配置的有效采样沿，无边沿文字；悬停查看时刻。方框为协议字段，红色为异常。SPI/UART/I²C/SWD 显示位名=值、位边界和三角形采样点。PCIe 页为 TLP 事务接口示意。"}]},
            {"title": "字段记录", "span": 12, "widgets": [{"type": "table", "source": source, "rows_field": "rows", "columns": [
                {"field": "time", "label": "开始", "unit": "s"}, {"field": "duration", "label": "持续", "unit": "s"},
                {"field": "channel", "label": "通道"}, {"field": "label", "label": "字段"},
                {"field": "status", "label": "状态", "tones": {"error": "error"}}, {"field": "detail", "label": "内容 / 方向"}]}]},
            {"title": "导出本次仿真", "span": 12, "subtitle": "VCD 可供 GTKWave 等波形工具读取，JSON 保留参数、波形和解码。目标文件须不存在；PCIe 包字段保存在 JSON 中。", "widgets": [
                text(prefix+"_export_path", "文件路径", ""), combo(prefix+"_format", "格式", ["vcd", "json"]),
                {"type": "button", "label": "导出", "fit": True, "style": "secondary", "action": {"method": "export", "args": {
                    "protocol": protocol, "path": "$form."+prefix+"_export_path", "format": "$form."+prefix+"_format"}}, "publish": prefix+"_export"},
                {"type": "readout", "label": "已导出", "source": prefix+"_export", "field": "path"}]}
        ]})
    (Path(__file__).resolve().parents[1] / "page.json").write_text(json.dumps(page, ensure_ascii=False, indent=1)+"\n", encoding="utf-8")


if __name__ == "__main__":
    main()
