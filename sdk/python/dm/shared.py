"""Process-wide objects shared between Python plugins.

Simulators use it to co-simulate: e.g. the 6571 simulator and the FPGA platform
simulator attach to the same virtual chip object by a name from the station file::

    chip = dm.shared.setdefault("k7chip", lambda: SimChip())   # first one creates it
    chip = dm.shared.get("k7chip")                              # others look it up

Only for simulation glue — real instruments talk through the plugin API.
"""

import threading

_lock = threading.Lock()
_objects = {}


def get(name, default=None):
    with _lock:
        return _objects.get(name, default)


def put(name, obj):
    with _lock:
        _objects[name] = obj
    return obj


def setdefault(name, factory):
    with _lock:
        if name not in _objects:
            _objects[name] = factory()
        return _objects[name]


def remove(name):
    with _lock:
        _objects.pop(name, None)
