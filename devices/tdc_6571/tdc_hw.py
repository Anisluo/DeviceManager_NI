"""NI-Digital (nimi-python) backend for the TDC program on a PXIe-6571.

Loads the project's pin map, specifications, levels and timing, then bursts one pattern at a
time (unload_all_patterns + load_pattern), so edited variants can carry the same start label
as the pattern they were made from. Bursts run without blocking: poll done().
"""

import os


class Hardware:
    def __init__(self, resource, project, specs_path=None, driver_simulate=False):
        try:
            import hightime
            import nidigital
        except ImportError as e:
            raise RuntimeError("未安装 nidigital / hightime（pip install nidigital），或 NI-Digital 驱动不可用") from e
        self.nidigital, self.hightime = nidigital, hightime
        options = {"Simulate": True, "DriverSetup": {"Model": "6571"}} if driver_simulate else {}
        self.s = nidigital.Session(resource_name=resource, options=options)
        self.project = project
        self.loaded = None
        self.specs_loaded = []
        self.s.load_pin_map(project.pinmap_path)
        self.apply(specs_path or project.specs_path)

    def apply(self, specs_path):
        """(Re)load specifications, levels and timing, then apply the levels and timing sheets."""
        p = self.project
        if self.specs_loaded:
            try:
                self.s.unload_specifications(self.specs_loaded)
            except Exception:  # noqa: BLE001 — older drivers: reload over the old sheets
                pass
        specs = [specs_path] if specs_path else None
        self.s.load_specifications_levels_and_timing(specs, [p.levels_path] if p.levels_path else None,
                                                     [p.timing_path] if p.timing_path else None)
        self.specs_loaded = specs or []
        if p.levels_path and p.timing_path:
            sheet = lambda path: os.path.splitext(os.path.basename(path))[0]
            self.s.apply_levels_and_timing(sheet(p.levels_path), sheet(p.timing_path))

    def start(self, pattern_path, label):
        if self.loaded != pattern_path:
            self.s.unload_all_patterns()
            self.s.load_pattern(pattern_path)
            self.loaded = pattern_path
        self.s.burst_pattern(label, select_digital_function=True, wait_until_done=False)

    def done(self):
        return bool(self.s.is_done())

    def result(self, pins):
        try:
            sites = self.s.get_site_pass_fail()
            passed = all(sites.values()) if isinstance(sites, dict) else bool(sites)
        except Exception:  # noqa: BLE001
            passed = None
        fails = {}
        for pin in pins:
            try:
                fc = self.s.pins[pin].get_fail_count()
                fails[pin] = int(fc[0] if isinstance(fc, (list, tuple)) else fc)
            except Exception:  # noqa: BLE001 — pin not in the pattern
                pass
        return passed, fails

    def abort(self):
        self.s.abort()

    def close(self):
        try:
            self.s.close()
        except Exception:  # noqa: BLE001
            pass
