"""Glue used by the C++ host. Not part of the public API."""

import importlib.util
import os
import sys
import traceback

import _dmhost


class Writer:
    """sys.stdout / sys.stderr replacement that forwards to the host."""

    def __init__(self, stream):
        self.stream = stream

    def write(self, text):
        if text:
            _dmhost.write(self.stream, text)
        return len(text)

    def flush(self):
        pass

    def isatty(self):
        return False


def install_streams():
    sys.stdout = Writer("stdout")
    sys.stderr = Writer("stderr")


PRELUDE = ("import dm\n"
           "import dm.data\n"
           "from dm import device, devices, record, sleep, log, progress, linspace, sweep\n"
           "from dm import new_run, Axis, Scan, shmoo, DeviceError\n"
           "import math, time\n")


def new_namespace(filename="<script>"):
    ns = {"__name__": "__main__", "__file__": filename}
    exec(PRELUDE, ns)
    return ns


def _report(exc, filename):
    """Print a traceback without the host's own frames; return a one-line summary."""
    if isinstance(exc, KeyboardInterrupt):
        return "已停止"
    tb = exc.__traceback__
    while tb is not None and tb.tb_frame.f_code.co_filename == __file__:
        tb = tb.tb_next
    sys.stderr.write("".join(traceback.format_exception(type(exc), exc, tb)))
    return f"{type(exc).__name__}: {exc}"


def _failed(exc, filename):
    try:
        return False, _report(exc, filename)
    except BaseException:  # noqa: BLE001 — a second Stop while reporting the first
        return False, "已停止"


def run_script(code, filename, ns):
    """Run a sequence file. Returns (ok, message)."""
    try:
        ns["__file__"] = filename
        script_dir = os.path.dirname(filename)
        if script_dir and script_dir not in sys.path:
            sys.path.insert(0, script_dir)
        exec(compile(code, filename, "exec"), ns)
        return True, "完成"
    except BaseException as e:  # noqa: BLE001
        return _failed(e, filename)


def run_console(source, ns):
    """Run console input; single expressions echo their value like the Python REPL."""
    try:
        try:
            code = compile(source, "<console>", "single")
        except SyntaxError:
            code = compile(source, "<console>", "exec")
        exec(code, ns)
        return True, ""
    except BaseException as e:  # noqa: BLE001
        return _failed(e, "<console>")


def load_plugin_class(path, module_name, class_name):
    """Import <path> as a fresh module (so hot reload re-executes it) and return the class."""
    plugin_dir = os.path.dirname(path)
    # Drop the plugin's own helper modules (e.g. swd.py next to the entry file) so a reload
    # picks up their edits too.
    for name, mod in list(sys.modules.items()):
        f = getattr(mod, "__file__", None) or ""
        if f and os.path.dirname(os.path.abspath(f)) == os.path.abspath(plugin_dir):
            del sys.modules[name]
    added = plugin_dir not in sys.path
    if added:
        sys.path.insert(0, plugin_dir)
    try:
        spec = importlib.util.spec_from_file_location(module_name, path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
    finally:
        if added:
            sys.path.remove(plugin_dir)
    if class_name:
        return getattr(module, class_name)
    from dm.plugin import Plugin
    candidates = [v for v in vars(module).values()
                  if isinstance(v, type) and issubclass(v, Plugin) and v is not Plugin]
    if len(candidates) != 1:
        raise ImportError(f"{path}: specify the class as 'module:Class' (found {len(candidates)} Plugin subclasses)")
    return candidates[0]


def create_plugin(cls, config_json):
    import json
    return cls(json.loads(config_json) if config_json else {})


def format_exception(exc):
    """Cause first (shown in the device's error banner), full traceback after it."""
    summary = f"{type(exc).__name__}: {exc}"
    return summary + "\n\n" + "".join(traceback.format_exception(type(exc), exc, exc.__traceback__)).strip()
