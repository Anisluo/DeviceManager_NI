"""Persistent Vivado / Vivado Lab Tcl session for JTAG work (program, XADC, JTAG-to-AXI).

One `vivado -mode tcl` process is started on first use and kept alive (startup takes
tens of seconds). Each command is wrapped in `catch` and terminated by a sentinel
line, so results and Tcl errors come back reliably.

NOTE: written against the Vivado hardware-manager Tcl API without a board at hand;
property names marked VERIFY are checked by tools/verify_fpga.py on the 8881.
"""

import glob
import os
import queue
import subprocess
import threading
import time
import uuid

SEARCH = [
    "C:/AMDDesignTools/*/Vivado/bin/vivado.bat",
    "C:/AMDDesignTools/*/Vivado_Lab/bin/vivado_lab.bat",
    "C:/Xilinx/Vivado/*/bin/vivado.bat",
    "C:/Xilinx/Vivado_Lab/*/bin/vivado_lab.bat",
    "D:/AMDDesignTools/*/Vivado/bin/vivado.bat",
    "D:/Xilinx/Vivado/*/bin/vivado.bat",
    "E:/AMDDesignTools/*/Vivado/bin/vivado.bat",
    "E:/Xilinx/Vivado/*/bin/vivado.bat",
]


def find_vivado(hint=None):
    if hint and os.path.exists(hint):
        return hint
    for pattern in SEARCH:
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[-1]                    # newest version
    return None


def tcl_path(p):
    return "{" + os.path.abspath(p).replace("\\", "/") + "}"


class TclError(Exception):
    pass


class Vivado:
    def __init__(self, exe=None, log=None, startup_timeout=180):
        self.exe = find_vivado(exe)
        if not self.exe:
            raise RuntimeError("找不到 Vivado / Vivado Lab（options.vivado 指定 vivado.bat 路径）")
        self.log = log or (lambda line: None)
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.p = subprocess.Popen([self.exe, "-mode", "tcl", "-nolog", "-nojournal", "-notrace"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, encoding="utf-8", errors="replace", bufsize=1, creationflags=flags)
        self.lines = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()
        self.run("puts dm_ready", timeout=startup_timeout)

    def _reader(self):
        for line in self.p.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def run(self, tcl, timeout=120):
        """Run Tcl; returns its result string, raises TclError with Vivado's message."""
        tok = uuid.uuid4().hex[:8]
        wrapped = ("if {[catch {" + tcl + "} __dm_r]} {puts \"@@DM_ERR " + tok + " [string map {\\n { | }} $__dm_r]\"}"
                   " else {puts \"@@DM_OK " + tok + " [string map {\\n { | }} $__dm_r]\"}\n")
        self.p.stdin.write(wrapped)
        self.p.stdin.flush()
        deadline = time.time() + timeout
        out = []
        while True:
            try:
                line = self.lines.get(timeout=max(0.1, deadline - time.time()))
            except queue.Empty:
                raise TclError(f"Vivado 无响应（{timeout} s）：{tcl[:80]}")
            if line is None:
                raise TclError("Vivado 进程已退出：" + " | ".join(out[-5:]))
            if line.startswith("@@DM_OK " + tok):
                return line[len("@@DM_OK ") + len(tok) + 1:]
            if line.startswith("@@DM_ERR " + tok):
                raise TclError(line[len("@@DM_ERR ") + len(tok) + 1:])
            out.append(line)
            self.log(line)
            if time.time() > deadline:
                raise TclError(f"Vivado 超时：{tcl[:80]}")

    def close(self):
        try:
            self.p.stdin.write("exit\n")
            self.p.stdin.flush()
            self.p.wait(10)
        except Exception:  # noqa: BLE001
            self.p.kill()
        for pipe in (self.p.stdin, self.p.stdout):
            try:
                pipe.close()
            except Exception:  # noqa: BLE001
                pass


class HwSession:
    """Hardware manager on one JTAG target with an XC7K480T."""

    def __init__(self, vivado, url="localhost:3121", target="", part="xc7k480t"):
        self.v = vivado
        self.part = part
        self.v.run("open_hw_manager")
        self.v.run(f"connect_hw_server -url {url} -allow_non_jtag", timeout=60)
        flt = f"*{target}*" if target else "*"
        self.target = self.v.run(f"set t [lindex [get_hw_targets {flt}] 0]; open_hw_target $t; set t", timeout=60)
        self.device = self.v.run(f"set d [lindex [get_hw_devices {part}*] 0]; current_hw_device $d; "
                                 f"refresh_hw_device -update_hw_probes false $d; set d", timeout=60)
        if not self.device:
            raise TclError(f"JTAG 链上没有 {part}：{self.v.run('get_hw_devices')}")

    def prop(self, name, obj="[current_hw_device]"):
        return self.v.run(f"get_property {name} {obj}")

    def info(self):
        out = {"target": self.target, "device": self.device}
        for key, name in (("idcode", "IDCODE_HEX"), ("part", "PART"), ("done", "REGISTER.CONFIG_STATUS.BIT14_DONE_PIN")):
            try:
                out[key] = self.prop(name)                                     # VERIFY property names
            except TclError as e:
                out[key] = f"? ({e})"
        return out

    def program(self, bitstream, probes=None):
        cmds = [f"set_property PROGRAM.FILE {tcl_path(bitstream)} [current_hw_device]"]
        if probes:
            cmds.append(f"set_property PROBES.FILE {tcl_path(probes)} [current_hw_device]")
        cmds += ["program_hw_devices [current_hw_device]", "refresh_hw_device [current_hw_device]"]
        self.v.run("; ".join(cmds), timeout=300)
        return self.info()

    def xadc(self):
        r = self.v.run("set s [lindex [get_hw_sysmons -of_objects [current_hw_device]] 0]; refresh_hw_sysmon $s; "
                       "list [get_property TEMPERATURE $s] [get_property VCCINT $s] [get_property VCCAUX $s] "
                       "[get_property VCCBRAM $s]")                         # VERIFY sysmon property names
        vals = [float(x) for x in r.split()]
        return dict(zip(("temperature", "vccint", "vccaux", "vccbram"), vals))

    # ---- JTAG-to-AXI (needs a JTAG-to-AXI Master + rtl/dm_axil_adapter.v in the design) ----
    def axi_read(self, addr):
        r = self.v.run(f"set a [lindex [get_hw_axis] 0]; "
                       f"create_hw_axi_txn -force dm_rd $a -address {addr:08X} -len 1 -type read; "
                       f"run_hw_axi -quiet dm_rd; get_property DATA [get_hw_axi_txns dm_rd]")
        return int(r.strip(), 16)

    def axi_write(self, addr, value):
        self.v.run(f"set a [lindex [get_hw_axis] 0]; "
                   f"create_hw_axi_txn -force dm_wr $a -address {addr:08X} -data {value & 0xFFFFFFFF:08X} -len 1 -type write; "
                   f"run_hw_axi -quiet dm_wr")


class SimHw:
    """Stand-in for HwSession on the simulated board."""

    def __init__(self, board):
        self.board = board
        self.target = "localhost:3121/xilinx_tcf/Digilent/SIM"
        self.device = "xc7k480t_0"

    def info(self):
        return {"target": self.target, "device": self.device, "idcode": "03656093", "part": "xc7k480tffg901-3",
                "done": "1" if self.board.configured else "0"}

    def program(self, bitstream, probes=None):
        time.sleep(0.3)
        self.board.program(bitstream)
        return self.info()

    def xadc(self):
        return self.board.xadc()

    def axi_read(self, addr):
        return self.board.reg_read(addr)

    def axi_write(self, addr, value):
        self.board.reg_write(addr, value)
