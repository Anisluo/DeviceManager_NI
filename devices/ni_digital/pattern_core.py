"""Project editor of the 6571 page: the runner DLL of the TDC measurement task, loaded by ctypes.

The project model (pin map, levels, timing, compiled patterns, export) exists once, in C++. The
driver loads that DLL through its plain C ABI and forwards the page's "pe.<method>" calls to it.
Instrument calls come back through the host callback and run on this driver, so the page can
also burst what it edits.

The DLL is loaded from a copy: the host's hot reload needs the built file to stay writable.
"""

import ctypes
import glob
import json
import os
import shutil

BUFFER = 1 << 20
E_BUFFER = -2

EVENT_FN = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)
SINK_FN = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)
HOST_FN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                           ctypes.c_char_p, SINK_FN, ctypes.c_void_p)


class HostApi(ctypes.Structure):
    _fields_ = [("size", ctypes.c_int), ("ctx", ctypes.c_void_p), ("invoke", HOST_FN)]


class PatternCore:
    def __init__(self, library, config, device_call, emit, work_dir):
        os.makedirs(work_dir, exist_ok=True)
        for old in glob.glob(os.path.join(work_dir, "pattern_core_*" + os.path.splitext(library)[1])):
            try:
                os.remove(old)
            except OSError:
                pass                                    # another instance has it loaded
        copy = os.path.join(work_dir, f"pattern_core_{os.getpid()}_{id(self)}{os.path.splitext(library)[1]}")
        shutil.copyfile(library, copy)
        self.lib = ctypes.CDLL(copy)
        self.lib.plugin_create.restype = ctypes.c_void_p
        self.lib.plugin_create.argtypes = [ctypes.c_char_p]
        self.lib.plugin_invoke.restype = ctypes.c_int
        self.lib.plugin_invoke.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        self.lib.plugin_destroy.argtypes = [ctypes.c_void_p]
        self.lib.plugin_set_host.argtypes = [ctypes.c_void_p, ctypes.POINTER(HostApi)]
        self.lib.plugin_set_callback.argtypes = [ctypes.c_void_p, EVENT_FN, ctypes.c_void_p]

        def on_host(_ctx, _device, method_name, args, sink, user):
            rc, text = device_call(method_name.decode(), args.decode() if args else "")
            sink(text.encode(), user)
            return rc

        self._on_host = HOST_FN(on_host)                # kept alive as long as the handle
        self._on_event = EVENT_FN(lambda evt, _user: emit(evt.decode()))
        self.handle = self.lib.plugin_create(json.dumps(config).encode())
        if not self.handle:
            raise RuntimeError("无法打开图形工程：" + str(config.get("options", {}).get("project")))
        self._api = HostApi(ctypes.sizeof(HostApi), None, self._on_host)
        self.lib.plugin_set_host(self.handle, ctypes.byref(self._api))
        self.lib.plugin_set_callback(self.handle, self._on_event, None)

    def invoke(self, method_name, args_json):
        """Returns (rc, json_text), rc as plugin_invoke."""
        out = ctypes.create_string_buffer(BUFFER)
        rc = self.lib.plugin_invoke(self.handle, method_name.encode(), (args_json or "{}").encode(), out, BUFFER)
        if rc != E_BUFFER:
            return min(rc, 0), out.value.decode()
        need = json.loads(out.value)["need"]
        out = ctypes.create_string_buffer(need)
        rc = self.lib.plugin_invoke(self.handle, b"$fetch", b"{}", out, need)
        return min(rc, 0), out.value.decode()

    def close(self):
        if self.handle:
            self.lib.plugin_destroy(self.handle)
            self.handle = None
