"""Deterministic protocol transactions for the host's logic analyzer viewer."""
import json
import math
import re
from pathlib import Path

from dm.plugin import Plugin, method


def integer(value, low, high, name):
    try:
        n = int(value, 0) if isinstance(value, str) else int(value)
        if not isinstance(value, str) and n != value:
            raise ValueError()
    except (TypeError, ValueError, OverflowError):
        raise ValueError(name + " 必须是整数") from None
    if not low <= n <= high:
        raise ValueError(f"{name} 范围 {low}…{high}")
    return n


def payload(value, limit=64):
    if isinstance(value, str):
        words = re.split(r"[\s,;]+", value.strip()) if value.strip() else []
        try:
            data = [int(w, 16) for w in words]
        except ValueError:
            raise ValueError("数据填写空格分隔的十六进制字节，例如 A5 5A 01") from None
    elif isinstance(value, list):
        data = [integer(v, 0, 255, "字节") for v in value]
    else:
        raise ValueError("数据必须为十六进制字节字符串或数组")
    if not 1 <= len(data) <= limit or any(v < 0 or v > 255 for v in data):
        raise ValueError(f"数据须为 1…{limit} 个字节，每字节 00…FF")
    return data


def bits(value, width, lsb=False):
    return [(value >> i) & 1 for i in (range(width) if lsb else reversed(range(width)))]


class Trace:
    def __init__(self, frequency):
        self.period = 1 / frequency
        self.time = 0.0
        self.lanes = {}
        self.events = []

    def lane(self, name, initial=0, note=""):
        self.lanes[name] = {"name": name, "note": note, "lo": 0, "hi": 1, "unit": "",
                            "t": [0.0], "v": [initial], "annotations": []}

    def set(self, name, value, when=None):
        lane = self.lanes[name]
        t = self.time if when is None else when
        if t < lane["t"][-1] - 1e-18:
            raise ValueError("non-monotonic trace")
        if t == lane["t"][-1]:
            lane["v"][-1] = value
        elif value != lane["v"][-1]:
            lane["t"].append(t)
            lane["v"].append(value)

    def wait(self, cycles=1):
        self.time += cycles * self.period

    def bus(self, name, value, width=32):
        self.lanes[name]["bus"] = True
        self.lanes[name]["width"] = width
        self.set(name, value)

    def event(self, lane, start, label, detail="", status="ok"):
        item = {"t": start, "end": self.time, "label": label, "detail": detail, "status": status}
        self.lanes[lane]["annotations"].append(item)
        self.events.append(dict(item, channel=lane, time=start, duration=self.time-start))

    def bit(self, lane, start, end, sample, label, value, field):
        self.lanes[lane].setdefault("bits", []).append({"t": start, "end": end, "sample": sample,
            "label": label, "value": int(value), "field": field})

    def clock_bit(self, clk, data, value):
        self.set(clk, 0)
        self.set(data, value)
        self.wait(.5)
        self.set(clk, 1)
        self.wait(.5)


def spi(o, rate):
    data = payload(o.get("data", "9F 00 00 00"))
    rx = payload(o.get("response", "00 EF 40 18"))
    if len(rx) != len(data):
        raise ValueError("SPI 发送和响应字节数须一致")
    mode = integer(o.get("mode", 0), 0, 3, "SPI mode")
    order = o.get("order", "MSB")
    if order not in ("MSB", "LSB"):
        raise ValueError("位序须为 MSB 或 LSB")
    cpol, cpha = mode >> 1, mode & 1
    t = Trace(rate)
    for n, initial in (("CS#", 1), ("SCLK", cpol), ("MOSI", 0), ("MISO", 0)):
        t.lane(n, initial)
    t.wait(); t.set("CS#", 0); t.wait(.5)
    for byte_index, (tx, response) in enumerate(zip(data, rx)):
        start = t.time
        for bit_index, (a, b) in enumerate(zip(bits(tx, 8, order == "LSB"), bits(response, 8, order == "LSB"))):
            bit_start = t.time
            if cpha == 0:
                t.set("MOSI", a); t.set("MISO", b)
                t.wait(.5); t.set("SCLK", 1-cpol)
                t.wait(.5); t.set("SCLK", cpol)
            else:
                t.set("SCLK", 1-cpol); t.set("MOSI", a); t.set("MISO", b)
                t.wait(.5); t.set("SCLK", cpol); t.wait(.5)
            for lane, value in (("MOSI", a), ("MISO", b)):
                index = bit_index if order == "LSB" else 7-bit_index
                t.bit(lane, bit_start, t.time, bit_start+t.period*.5, f"D{index}", value,
                      f"Byte {byte_index} / {order} / mode {mode}")
        t.event("MOSI", start, f"TX {tx:02X}", f"mode {mode} / {order}")
        t.event("MISO", start, f"RX {response:02X}")
    t.wait(.5); t.set("CS#", 1); t.wait()
    return t, "SPI 全双工；CS# 低有效，按 CPOL/CPHA 指定边沿采样。"


def uart(o, rate):
    data = payload(o.get("data", "48 65 6C 6C 6F"))
    width = integer(o.get("data_bits", 8), 5, 8, "数据位")
    stop = integer(o.get("stop_bits", 1), 1, 2, "停止位")
    parity = o.get("parity", "none")
    fault = o.get("fault", "none")
    if parity not in ("none", "even", "odd") or fault not in ("none", "parity", "stop"):
        raise ValueError("UART 校验或故障类型无效")
    if fault == "parity" and parity == "none":
        raise ValueError("注入校验错误前请选择 even 或 odd")
    if any(v >= 1 << width for v in data):
        raise ValueError("数据超出所选数据位宽")
    t = Trace(rate); t.lane("TX", 1, "LSB first"); t.wait(2)
    for index, value in enumerate(data):
        start = t.time; t.set("TX", 0); t.wait()
        t.event("TX", start, "START")
        start = t.time
        for index_bit, bit in enumerate(bits(value, width, True)):
            bit_start = t.time; t.set("TX", bit); t.wait()
            t.bit("TX", bit_start, t.time, bit_start+t.period*.5, f"D{index_bit}", bit, f"Byte {index}")
        t.event("TX", start, f"0x{value:02X}", f"{width} data bits / LSB first")
        bad = index == 0 and fault != "none"
        if parity != "none":
            start = t.time
            p = (value.bit_count() & 1) ^ (parity == "odd")
            if bad and fault == "parity": p ^= 1
            t.set("TX", int(p)); t.wait()
            t.event("TX", start, "PARITY", parity, "error" if bad and fault == "parity" else "ok")
        start = t.time; t.set("TX", 0 if bad and fault == "stop" else 1); t.wait(stop)
        t.event("TX", start, "STOP", f"{stop} stop bits", "error" if bad and fault == "stop" else "ok")
        t.set("TX", 1); t.wait()
    t.wait()
    return t, "UART 异步帧；空闲高，起始低，LSB 优先。故障只注入第一帧。"


def i2c(o, rate):
    address = integer(o.get("address", 0x50), 0, 127, "I²C 地址")
    data = payload(o.get("data", "00 A5 5A"))
    operation = o.get("operation", "write")
    if operation not in ("write", "read"):
        raise ValueError("I²C 操作须为 write 或 read")
    nack = integer(o.get("nack_at", -1), -1, len(data), "NACK 字节序号")
    stretch = integer(o.get("stretch", 0), 0, 100, "时钟延展周期")
    t = Trace(rate); t.lane("SCL", 1, "open drain + pull-up"); t.lane("SDA", 1, "0=拉低 / 1=释放")
    t.wait(); start = t.time; t.set("SDA", 0); t.wait(.5); t.set("SCL", 0)
    t.event("SDA", start, "START")
    stream = [(address << 1) | (operation == "read")] + data
    for index, byte in enumerate(stream):
        start = t.time
        for index_bit, bit in enumerate(bits(byte, 8)):
            bit_start = t.time
            t.set("SDA", bit); t.wait(.25)
            if index == 1 and stretch and t.time - start < t.period:
                s = t.time; t.wait(stretch); t.event("SCL", s, "STRETCH")
            sample_at = t.time
            t.set("SCL", 1); t.wait(.5); t.set("SCL", 0); t.wait(.25)
            label = (f"A{6-index_bit}" if index_bit < 7 else "R/W") if index == 0 else f"D{7-index_bit}"
            t.bit("SDA", bit_start, t.time, sample_at, label, bit, "ADDRESS" if index == 0 else f"Byte {index-1}")
        t.event("SDA", start, f"ADDR {address:02X} {'R' if operation == 'read' else 'W'}" if index == 0 else f"DATA {byte:02X}",
                "master → slave" if index == 0 or operation == "write" else "slave → master")
        start = t.time
        no_ack = index == nack or (operation == "read" and index == len(stream)-1)
        t.set("SDA", int(no_ack)); t.wait(.25); t.set("SCL", 1); t.wait(.5); t.set("SCL", 0); t.wait(.25)
        t.event("SDA", start, "NACK" if no_ack else "ACK", "master" if operation == "read" and index else "slave",
                "error" if index == nack else "ok")
        if no_ack: break
    t.set("SDA", 0); t.wait(.25); t.set("SCL", 1); t.wait(.25)
    start = t.time; t.set("SDA", 1); t.wait(.5); t.event("SDA", start, "STOP"); t.wait()
    return t, "I²C 7 位地址；含 START/STOP、逐字节 ACK/NACK 和可选时钟延展。读事务最后一字节由主机 NACK。"


def swd(o, rate):
    read = o.get("operation", "read") == "read"
    if o.get("operation", "read") not in ("read", "write"):
        raise ValueError("SWD 操作须为 read 或 write")
    ap = integer(o.get("ap", 0), 0, 1, "APnDP")
    addr = integer(o.get("address", 0), 0, 12, "寄存器地址")
    if addr % 4: raise ValueError("SWD 地址须为 0、4、8、12")
    value = integer(o.get("value", 0x2BA01477), 0, 0xFFFFFFFF, "32 位数据")
    ack = o.get("ack", "OK")
    if ack not in ("OK", "WAIT", "FAULT"): raise ValueError("SWD ACK 无效")
    parity_error = integer(o.get("parity_error", 0), 0, 1, "校验错误")
    reset = integer(o.get("line_reset", 1), 0, 1, "线路复位")
    t = Trace(rate)
    t.lane("SWCLK"); t.lane("SWDIO", 1); t.lane("HOST_OE", 1, "1=主机驱动 / 0=释放")
    t.wait()
    def send(seq, label, owner, detail="", status="ok"):
        start = t.time; t.set("HOST_OE", int(owner == "host"))
        for i, b in enumerate(seq):
            bit_start = t.time; t.clock_bit("SWCLK", "SWDIO", b)
            if label == "REQUEST":
                bit_label = ["Start", "AP", "RnW", "A2", "A3", "P", "Stop", "Park"][i]
            else:
                bit_label = f"D{i}" if label.startswith("DATA") else str(i)
            if label == "REQUEST" or label.startswith(("DATA", "ACK")):
                t.bit("SWDIO", bit_start, t.time, bit_start+t.period*.5, bit_label, b, label)
        t.set("SWCLK", 0)
        t.lanes["SWDIO"].setdefault("states", []).append({"t": start, "end": t.time,
            "state": "X" if owner == "none" else "1", "timeset": owner})
        t.event("SWDIO", start, label, detail or owner, status)
    if reset:
        send([1]*56, "LINE RESET", "host")
        send([0, 0], "IDLE", "host")
    fields = [ap, int(read), (addr >> 2) & 1, (addr >> 3) & 1]
    request = [1] + fields + [sum(fields) & 1, 0, 1]
    send(request, "REQUEST", "host", f"{'AP' if ap else 'DP'} {'READ' if read else 'WRITE'} @0x{addr:X}")
    send([1], "TURN", "none")
    send(bits({"OK": 1, "WAIT": 2, "FAULT": 4}[ack], 3, True), "ACK " + ack, "target", status="ok" if ack == "OK" else "error")
    if ack == "OK":
        if not read: send([1], "TURN", "none")
        send(bits(value, 32, True), f"DATA {value:08X}", "target" if read else "host")
        send([(value.bit_count() & 1) ^ parity_error], "PARITY", "target" if read else "host", status="error" if parity_error else "ok")
        if read: send([1], "TURN", "none")
    else:
        send([1], "TURN", "none")
    send([0]*8, "IDLE", "host"); t.wait()
    return t, "SWD 单次传输；请求、周转、ACK、32 位 LSB 数据和偶校验。WAIT/FAULT 不进入数据阶段；从已经处于 SWD 模式的接口开始，未实现 JTAG 切换或 AP posted-read 管线。"


def crc15(seq):
    crc = 0
    for bit in seq:
        feedback = ((crc >> 14) & 1) ^ bit
        crc = (crc << 1) & 0x7FFF
        if feedback: crc ^= 0x4599
    return crc


def can(o, rate):
    ident = integer(o.get("identifier", 0x123), 0, 0x7FF, "CAN 标准帧 ID")
    data = payload(o.get("data", "11 22 33 44"), 8)
    ack = integer(o.get("ack", 1), 0, 1, "CAN ACK")
    error = integer(o.get("crc_error", 0), 0, 1, "CRC 错误")
    fields = [("SOF", [0]), (f"ID {ident:03X}", bits(ident, 11)), ("RTR/IDE/r0", [0, 0, 0]),
              (f"DLC {len(data)}", bits(len(data), 4))]
    fields += [(f"DATA {v:02X}", bits(v, 8)) for v in data]
    checksum = crc15([b for _, seq in fields for b in seq])
    fields.append((f"CRC {checksum:04X}", bits(checksum ^ error, 15)))
    t = Trace(rate); t.lane("CAN", 1, "0=dominant / 1=recessive"); t.lane("TXD", 1); t.lane("RXD", 1)
    t.wait(3); last = None; run = 0; stuffed = 0
    def send(bit, tx=None):
        t.set("CAN", bit); t.set("RXD", bit); t.set("TXD", bit if tx is None else tx); t.wait()
    for label, seq in fields:
        start = t.time
        for bit in seq:
            send(bit)
            run = run + 1 if last == bit else 1; last = bit
            if run == 5:
                send(1-bit); last = 1-bit; run = 1; stuffed += 1
        t.event("CAN", start, label, "stuff bits included", "error" if error and label.startswith("CRC") else "ok")
    start = t.time; send(1); t.event("CAN", start, "CRC delimiter")
    if error:
        send(1, 1); send(1, 1)  # CRC error: no ACK; error flag follows ACK delimiter.
        start = t.time
        for b in [0]*6 + [1]*8: send(b, 1)
        t.event("CAN", start, "ACTIVE ERROR", "接收节点检测到 CRC 错误；示意错误标志及界定符", "error")
    else:
        start = t.time; send(0 if ack else 1, 1)
        t.event("CAN", start, "ACK" if ack else "ACK ERROR", "sender releases TXD", "ok" if ack else "error")
        if not ack:
            start = t.time
            for b in [0]*6 + [1]*8: send(b)
            t.event("CAN", start, "ACTIVE ERROR", "发送节点检测到 ACK 错误", "error")
        else:
            send(1); start = t.time
            for _ in range(7): send(1)
            t.event("CAN", start, "EOF")
    for _ in range(3): send(1)
    return t, f"Classic CAN 2.0A、11 位 ID、数据帧、CRC-15=0x{checksum:04X}、填充位 {stuffed} 个；不含 CAN FD、多节点仲裁或模拟差分电压。"


def axi(o, rate):
    address = integer(o.get("address", 0x40000000), 0, 0xFFFFFFFC, "地址")
    if address % 4: raise ValueError("AXI 地址须 4 字节对齐")
    words = o.get("data", "12345678 89ABCDEF")
    if not isinstance(words, str): raise ValueError("AXI 数据填写空格分隔的 32 位十六进制字")
    try: values = [int(w, 16) for w in words.split()]
    except ValueError: raise ValueError("AXI 数据须为十六进制字") from None
    if not 1 <= len(values) <= 16 or any(v < 0 or v > 0xFFFFFFFF for v in values):
        raise ValueError("AXI 数据须为 1…16 个 32 位字")
    if (address & 4095) + len(values)*4 > 4096: raise ValueError("AXI burst 不能跨越 4 KiB 边界")
    stall = integer(o.get("stall", 2), 0, 16, "背压周期")
    op = o.get("operation", "write")
    if op not in ("read", "write"): raise ValueError("AXI 操作须为 read 或 write")
    response = o.get("response", "OKAY")
    if response not in ("OKAY", "SLVERR", "DECERR"): raise ValueError("AXI 响应无效")
    t = Trace(rate); t.lane("ACLK"); t.lane("ARESETn", 0)
    channels = ("AWVALID", "AWREADY", "AWADDR", "WVALID", "WREADY", "WDATA", "WLAST", "BVALID", "BREADY", "BRESP") if op == "write" else ("ARVALID", "ARREADY", "ARADDR", "RVALID", "RREADY", "RDATA", "RLAST", "RRESP")
    for n in channels: t.lane(n)
    def cycle():
        t.wait(.5); t.set("ACLK", 1); t.wait(.5); t.set("ACLK", 0)
    cycle(); cycle(); t.set("ARESETn", 1); cycle()
    prefix = "AW" if op == "write" else "AR"
    start = t.time; t.set(prefix+"VALID", 1); t.bus(prefix+"ADDR", address)
    for _ in range(stall): cycle()
    t.set(prefix+"READY", 1); cycle()
    t.event(prefix+"ADDR", start, f"0x{address:08X}", f"INCR / AxLEN={len(values)-1} / AxSIZE=2")
    t.set(prefix+"VALID", 0); t.set(prefix+"READY", 0)
    d = "W" if op == "write" else "R"
    for i, value in enumerate(values):
        start = t.time; t.set(d+"VALID", 1); t.set(d+"LAST", int(i == len(values)-1)); t.bus(d+"DATA", value)
        if op == "read": t.bus("RRESP", {"OKAY": 0, "SLVERR": 2, "DECERR": 3}[response], 2)
        for _ in range(stall): cycle()
        t.set(d+"READY", 1); cycle()
        t.event(d+"DATA", start, f"0x{value:08X}", f"beat {i} @ 0x{address+4*i:08X}; VALID/READY 在上升沿同时为 1")
        if op == "read": t.event("RRESP", start, response, status="ok" if response == "OKAY" else "error")
        t.set(d+"VALID", 0); t.set(d+"READY", 0); t.set(d+"LAST", 0)
    if op == "write":
        start = t.time; t.set("BVALID", 1); t.bus("BRESP", {"OKAY": 0, "SLVERR": 2, "DECERR": 3}[response], 2)
        for _ in range(stall): cycle()
        t.set("BREADY", 1); cycle(); t.event("BRESP", start, response, status="ok" if response == "OKAY" else "error")
        t.set("BVALID", 0); t.set("BREADY", 0)
    cycle(); cycle()
    return t, "AXI4 单 ID、32 位、INCR burst；地址/数据/响应通道握手，VALID 在背压期间保持。总线数值以解码条显示；未建模并发及乱序。"


def pcie(o, rate):
    # Deliberately model transaction-interface cycles, not fabricated PCIe serial encoding.
    address = integer(o.get("address", 0x1000), 0, 0xFFFFFFFC, "PCIe 地址")
    if address % 4: raise ValueError("PCIe 地址须 DWORD 对齐")
    data = payload(o.get("data", "11 22 33 44"), 64)
    if len(data) % 4: raise ValueError("PCIe 示例负载须为 4 字节整数倍")
    if (address & 4095) + len(data) > 4096: raise ValueError("PCIe 请求不能跨越 4 KiB 边界")
    op = o.get("operation", "MRd")
    if op not in ("MRd", "MWr"): raise ValueError("PCIe 示例支持 MRd / MWr")
    latency = integer(o.get("latency", 8), 1, 100, "Completion 等待周期")
    t = Trace(rate)
    for n in ("USER_CLK", "TX_VALID", "TX_READY", "TX_SOP", "TX_EOP", "TX_TLP", "RX_VALID", "RX_READY", "RX_SOP", "RX_EOP", "RX_TLP"):
        t.lane(n, 1 if n.endswith("READY") else 0, "transaction interface")
    def cycle():
        t.wait(.5); t.set("USER_CLK", 1); t.wait(.5); t.set("USER_CLK", 0)
    for _ in range(2): cycle()
    start = t.time; t.set("TX_VALID", 1); t.set("TX_SOP", 1)
    cycle(); t.set("TX_SOP", 0)
    for _ in range(1 + (len(data)//4 if op == "MWr" else 0)): cycle()
    t.set("TX_EOP", 1); cycle()
    t.event("TX_TLP", start, f"{op} @0x{address:08X}", f"32-bit transaction stream / 3DW header / Length={len(data)//4} DW / Tag=0 / FirstBE=F / LastBE={'0' if len(data)==4 else 'F'}")
    t.set("TX_VALID", 0); t.set("TX_EOP", 0)
    if op == "MWr":
        t.events[-1]["detail"] += " / Payload=" + " ".join(f"{v:02X}" for v in data)
    else:
        for _ in range(latency): cycle()
        start = t.time; t.set("RX_VALID", 1); t.set("RX_SOP", 1)
        cycle(); t.set("RX_SOP", 0)
        for _ in range(1 + len(data)//4): cycle()
        t.set("RX_EOP", 1); cycle()
        t.event("RX_TLP", start, "CplD / Successful Completion", "Payload=" + " ".join(f"{v:02X}" for v in data))
        t.set("RX_VALID", 0); t.set("RX_EOP", 0)
    for _ in range(2): cycle()
    t.lanes["TX_TLP"]["bus"] = True
    t.lanes["RX_TLP"]["bus"] = True
    return t, "PCIe TLP 事务接口示意（USER_CLK），展示 Memory Read/Write 与 CplD 延迟。不是物理串行波形；未建模 LTSSM、8b/10b、128b/130b、DLLP、LCRC 或信号完整性。"


GENERATORS = {"SWD": swd, "SPI": spi, "I2C": i2c, "PCIE": pcie, "AXI": axi, "UART": uart, "CAN": can}
DEFAULT_RATE = {"SWD": 1e6, "SPI": 1e6, "I2C": 100e3, "PCIE": 125e6, "AXI": 50e6, "UART": 115200, "CAN": 500e3}


class ProtocolSim(Plugin):
    def __init__(self, config):
        super().__init__(config)
        self.revision = 0
        self.captures = {}
        self.settings = {}

    @method("生成指定协议的时序与解码记录；不连接硬件", name="simulate")
    def generate(self, protocol: str = "SPI", frequency: float = None, options: dict = None):
        protocol = protocol.upper()
        if protocol not in GENERATORS: raise ValueError("不支持的协议：" + protocol)
        if options is not None and not isinstance(options, dict): raise ValueError("options 必须为对象")
        try: frequency = float(DEFAULT_RATE[protocol] if frequency is None else frequency)
        except (TypeError, ValueError): raise ValueError("频率须为数值") from None
        if not math.isfinite(frequency) or not 1 <= frequency <= 1e9:
            raise ValueError("频率/波特率范围为 1 Hz…1 GHz")
        trace, description = GENERATORS[protocol](options or {}, frequency)
        clocks = {"SPI": ("SCLK", "MOSI"), "I2C": ("SCL", "SDA"), "SWD": ("SWCLK", "SWDIO")}
        if protocol in clocks:
            clock, data_lane = clocks[protocol]
            trace.lanes[clock]["samples"] = sorted({b["sample"] for b in trace.lanes[data_lane].get("bits", [])})
        elif protocol in ("AXI", "PCIE"):
            clock = trace.lanes["ACLK" if protocol == "AXI" else "USER_CLK"]
            clock["samples"] = [t for t, v in zip(clock["t"][1:], clock["v"][1:]) if v == 1]
        for lane in trace.lanes.values():
            lane["show_edges"] = bool(lane.get("samples")) and not lane.get("bus", False)
        self.revision += 1
        capture = {"protocol": protocol, "frequency": frequency, "lanes": list(trace.lanes.values()), "lane_height": 96,
                   "events": trace.events, "rows": trace.events, "t_end": trace.time,
                   "rev": self.revision, "description": description, "duration": trace.time,
                   "event_count": len(trace.events), "error_count": sum(e["status"] == "error" for e in trace.events)}
        self.captures[protocol] = capture
        self.settings[protocol] = {"protocol": protocol, "frequency": frequency, "options": options or {}}
        self.log(f"{protocol} 仿真完成：{len(trace.events)} 个字段，{trace.time*1e6:.3f} µs")
        return capture

    @method("读取每种协议最近一次仿真；首次读取生成默认示例")
    def capture(self, protocol: str = "SPI"):
        protocol = protocol.upper()
        return self.captures.get(protocol) or self.generate(protocol)

    @method("导出最后一次仿真：VCD 或 JSON，目标须不存在")
    def export(self, protocol: str, path: str, format: str = "vcd"):
        protocol = protocol.upper()
        if protocol not in self.captures: raise ValueError("请先运行该协议的仿真")
        if format not in ("vcd", "json"): raise ValueError("导出格式须为 vcd 或 json")
        if not path.strip(): raise ValueError("请填写导出路径")
        cap = self.captures[protocol]
        dest = Path(path).expanduser()
        content = json.dumps(dict(cap, settings=self.settings[protocol]), ensure_ascii=False, indent=2) if format == "json" else self._vcd(cap)
        dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open("x", encoding="utf-8") as f: f.write(content)
        return {"path": str(dest.resolve()), "format": format}

    @staticmethod
    def _vcd(cap):
        lines = ["$timescale 1 ps $end", "$scope module protocol_sim $end"]
        points = {}
        for i, lane in enumerate(cap["lanes"]):
            if lane.get("bus") and "width" not in lane:
                continue  # Packet annotations live in JSON; do not export a fictitious serial bus.
            code = f"s{i}"
            name = re.sub(r"[^A-Za-z0-9_]", "_", lane["name"])
            width = lane.get("width", 1)
            lines.append(f"$var wire {width} {code} {name} $end")
            for t, v in zip(lane["t"], lane["v"]):
                sample = f"{int(v)}{code}" if width == 1 else f"b{int(v):0{width}b} {code}"
                points.setdefault(round(t*1e12), []).append(sample)
        lines += ["$upscope $end", "$enddefinitions $end"]
        for tick in sorted(points): lines += [f"#{tick}"] + points[tick]
        lines.append(f"#{round(cap['t_end']*1e12)}")
        return "\n".join(lines) + "\n"
