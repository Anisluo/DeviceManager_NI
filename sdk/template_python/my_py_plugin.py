"""Python plugin template: copy sdk/template_python to devices/<id>/ and rename."""

import random

from dm.plugin import Plugin, PluginError, method, E_HW


class MyPyPlugin(Plugin):
    def __init__(self, config):
        super().__init__(config)          # sets self.config / resource / simulate / options
        self.level = 0.0
        # Real hardware: open the session for self.resource here (e.g. nidmm.Session(...)).
        # Raising here makes the device show the traceback as its error.

    @method("Set output level")
    def set_level(self, level: float):
        if not 0 <= level <= 5:
            raise ValueError("level must be in [0, 5]")      # -> DM_E_BAD_ARGS
        self.level = level                                   # returning None -> {"ok": true}

    @method("Read back output level")
    def get_level(self):
        return {"level": self.level}

    @method("Measure")
    def read(self):
        if not self.simulate:
            raise PluginError("hardware path not implemented", E_HW)
        return {"value": self.level + random.gauss(0, 1e-4)}

    def close(self):
        pass                                                 # release hardware here
