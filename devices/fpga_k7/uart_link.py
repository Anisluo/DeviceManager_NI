"""Host side of rtl/dm_uart_bridge.v.

Request  : A5 | CMD | SEQ | ADDR[4] LE | ARG | CRC8       (CMD 01 READ ARG=COUNT, 02 WRITE ARG=DATA[4])
Response : 5A | SEQ | N | DATA[4N] LE | STATUS | CRC8   (STATUS 0 OK, 1 CRC, 2 bad cmd, 3 bus timeout)
CRC-8/SMBUS (poly 0x07, init 0) over the bytes after the sync byte.

The transport is anything with write(bytes) and read(n, timeout) -> bytes (pyserial
port, or the simulated board), so the same code is exercised in simulation.
"""

import struct
import threading
import time

STATUS_TEXT = {0: "OK", 1: "CRC 错误", 2: "非法命令", 3: "总线超时"}


class LinkError(Exception):
    pass


def crc8(data, crc=0):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def encode_read(seq, addr, count):
    body = bytes([0x01, seq & 0xFF]) + struct.pack("<I", addr & 0xFFFFFFFF) + bytes([count])
    return b"\xA5" + body + bytes([crc8(body)])


def encode_write(seq, addr, value):
    body = bytes([0x02, seq & 0xFF]) + struct.pack("<II", addr & 0xFFFFFFFF, value & 0xFFFFFFFF)
    return b"\xA5" + body + bytes([crc8(body)])


class SerialTransport:
    """pyserial port (USB-UART of the FPGA board)."""

    def __init__(self, port, baud=921600):
        try:
            import serial
        except ImportError as e:
            raise RuntimeError("需要 pyserial：在宿主所用的 Python 中 pip install pyserial") from e
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.name = f"{port} @ {baud}"

    def write(self, data):
        self.s.write(data)

    def read(self, n, timeout):
        self.s.timeout = timeout
        return self.s.read(n)

    def flush_input(self):
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()


class UartLink:
    def __init__(self, transport, timeout=0.5, retries=2):
        self.t = transport
        self.timeout = timeout
        self.retries = retries
        self.seq = 0
        self.lock = threading.Lock()
        self.stats = {"transactions": 0, "retries": 0, "errors": 0}

    def _exchange(self, frame, expect_words):
        deadline = time.time() + self.timeout
        buf = b""

        def need(n):
            nonlocal buf
            while len(buf) < n:
                left = deadline - time.time()
                if left <= 0:
                    raise LinkError(f"超时：{len(buf)}/{n} 字节（FPGA 未配置、串口错误或波特率不符？）")
                buf += self.t.read(n - len(buf), left)

        self.t.write(frame)
        need(1)
        while buf[:1] != b"\x5A":                  # resync on garbage
            buf = buf[1:]
            need(1)
        need(3)
        n = buf[2]
        need(3 + 4 * n + 2)
        body = buf[1:3 + 4 * n + 1]
        if crc8(body) != buf[3 + 4 * n + 1]:
            raise LinkError("响应 CRC 错误")
        seq, status = buf[1], buf[3 + 4 * n]
        if seq != frame[2]:
            raise LinkError(f"序号不符：发 {frame[2]} 收 {seq}")
        words = list(struct.unpack(f"<{n}I", buf[3:3 + 4 * n])) if n else []
        if status != 0:
            raise LinkError(f"FPGA 返回状态 {status}（{STATUS_TEXT.get(status, '?')}）")
        if expect_words is not None and len(words) != expect_words:
            raise LinkError(f"返回 {len(words)} 个字，期望 {expect_words}")
        return words

    def _do(self, make_frame, expect):
        with self.lock:
            last = None
            for attempt in range(self.retries + 1):
                self.seq = (self.seq + 1) & 0xFF
                try:
                    if hasattr(self.t, "flush_input"):
                        self.t.flush_input()
                    r = self._exchange(make_frame(self.seq), expect)
                    self.stats["transactions"] += 1
                    return r
                except LinkError as e:
                    last = e
                    self.stats["retries"] += attempt < self.retries
            self.stats["errors"] += 1
            raise last

    def read(self, addr, count=1):
        out = []
        while count > 0:                              # the bridge takes up to 255 words per request
            n = min(count, 255)
            out += self._do(lambda s: encode_read(s, addr, n), n)
            addr += 4 * n
            count -= n
        return out

    def write(self, addr, value):
        self._do(lambda s: encode_write(s, addr, value), 0)
