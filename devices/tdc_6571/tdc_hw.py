"""NI-Digital (nimi-python) backend for the TDC program on a PXIe-6571.

Loads the project's pin map, specifications, levels and timing; timing sheets of generated
patterns are loaded the first time one of their patterns is burst, and the sheet holding the
pattern's timeset is applied before the burst. Patterns are burst one at a time
(unload_all_patterns + load_pattern), so edited variants can carry the same start label as the
pattern they were made from. Bursts run without blocking: poll done().
"""

import os


def _sheet(path):
    return os.path.splitext(os.path.basename(path))[0]


class Hardware:
    def __init__(self, resource, project, driver_simulate=False):
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
        p = project
        self.s.load_pin_map(p.pinmap_path)
        self.specs = [p.specs_path] if p.specs_path else []
        self.s.load_specifications_levels_and_timing(self.specs or None, [p.levels_path] if p.levels_path else None,
                                                     [p.timing_path] if p.timing_path else None)
        self.timing_loaded = {p.timing_path} if p.timing_path else set()
        self.timing_applied = None
        if p.timing_path:
            self.use_timing(p.timing_path)

    def use_timing(self, path):
        """Make the timing sheet in `path` the applied one (loading it first if needed)."""
        if path not in self.timing_loaded:
            self.s.load_specifications_levels_and_timing(None, None, [path])
            self.timing_loaded.add(path)
        if path != self.timing_applied and self.project.levels_path:
            self.s.apply_levels_and_timing(_sheet(self.project.levels_path), _sheet(path))
            self.timing_applied = path

    def set_specs(self, path):
        """Replace the specifications file (e.g. an edited copy) and re-apply levels and timing."""
        try:
            if self.specs:
                self.s.unload_specifications(self.specs)
        except Exception:  # noqa: BLE001 — older drivers: load over the old sheet
            pass
        self.s.load_specifications_levels_and_timing([path], None, None)
        self.specs = [path]
        if self.timing_applied and self.project.levels_path:
            self.s.apply_levels_and_timing(_sheet(self.project.levels_path), _sheet(self.timing_applied))

    def start(self, pattern_path, label, timing_path=None):
        if timing_path:
            self.use_timing(timing_path)
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
