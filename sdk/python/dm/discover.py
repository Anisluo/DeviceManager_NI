"""Instrument detection for stations with "discover": "ni".

ni() asks NI System Configuration (the same database NI MAX shows) for the hardware in the
chassis, through the `nisyscfg` Python package (pip install nisyscfg). The host maps each
model to a driver plugin via the "models" list in plugin.json.

Returns {"instruments": [{name, model, resource, slot, serial}], "note": "..."}.
"""


def _first(obj, *names):
    """First attribute among `names` that exists and is not empty (indexed properties are lists)."""
    for n in names:
        try:
            v = getattr(obj, n)
        except Exception:  # noqa: BLE001 — property not supported by this resource
            continue
        if isinstance(v, (list, tuple)):
            v = v[0] if v else None
        if v not in (None, ""):
            return v
    return None


def ni(target="localhost"):
    try:
        import nisyscfg
    except ImportError:
        return {"instruments": [], "note": "未安装 nisyscfg，无法检测机箱里的仪器（pip install nisyscfg）"}
    found = []
    try:
        with nisyscfg.Session(target=target) as session:
            for res in session.find_hardware():
                model = _first(res, "product_name", "expert_device_class")
                alias = _first(res, "expert_user_alias", "expert_resource_name")
                if not model or not alias:
                    continue
                if str(model).upper().startswith("PXIE-1") and "Chassis" in str(_first(res, "expert_device_class") or ""):
                    continue                                         # the chassis itself
                slot = _first(res, "slot_number")
                found.append({"name": str(alias), "model": str(model).replace("NI ", ""),
                              "resource": str(alias), "slot": int(slot) if slot not in (None, "") else None,
                              "serial": str(_first(res, "serial_number") or "")})
    except Exception as e:  # noqa: BLE001 — driver missing, remote target unreachable, ...
        return {"instruments": found, "note": f"NI 系统配置查询失败：{e}"}
    found.sort(key=lambda d: (d["slot"] is None, d["slot"] or 0, d["name"]))
    return {"instruments": found, "note": "" if found else "NI 系统配置没有找到仪器"}
