"""Minimal TDMS reader (no dependencies) for NI Digital Pattern Editor files.

.digipat / .digicapture / .tdms written by the editor are small little-endian, non-interleaved
TDMS files. This reads them into
  {"props": {...}, "ptypes": {...},
   "groups": {group: {"props": {...}, "ptypes": {...}, "channels": {name: [values, props, chunks, ptypes, dtype]}}}}
where chunks lists (byte offset, count, type) of every raw data block, so fixed-size values can be
patched in place (see patch_u32), and ptypes / dtype keep the TDMS types so write() can reproduce
a file with new values.
"""

import struct

TOC_META, TOC_RAW, TOC_NEW_LIST, TOC_INTERLEAVED, TOC_BIG_ENDIAN, TOC_DAQMX = 1 << 1, 1 << 3, 1 << 2, 1 << 5, 1 << 6, 1 << 7

_FIXED = {1: "b", 2: "h", 3: "i", 4: "q", 5: "B", 6: "H", 7: "I", 8: "Q", 9: "f", 10: "d", 0x21: "?"}
_STRING, _TIMESTAMP = 0x20, 0x44


class TdmsError(ValueError):
    pass


def _decode(b):
    for enc in ("utf-8", "gbk"):
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            pass
    return b.decode("utf-8", errors="replace")


class _Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.d, self.p)
        self.p += struct.calcsize("<" + fmt)
        return v[0] if len(v) == 1 else v

    def string(self):
        n = self.take("I")
        s = self.d[self.p:self.p + n]
        self.p += n
        return _decode(s)

    def value(self, dtype):
        if dtype == _STRING:
            return self.string()
        if dtype == _TIMESTAMP:
            frac, secs = self.take("Qq")
            return secs + frac / 2.0 ** 64
        if dtype not in _FIXED:
            raise TdmsError(f"unsupported TDMS property type 0x{dtype:X}")
        return self.take(_FIXED[dtype])


def _split_path(path):
    """"/'Group'/'Channel'" -> ("Group", "Channel"); "/" -> (None, None)."""
    parts, cur, i = [], None, 0
    while i < len(path):
        c = path[i]
        if c == "'":
            if cur is None:
                cur = ""
            elif i + 1 < len(path) and path[i + 1] == "'":
                cur += "'"
                i += 1
            else:
                parts.append(cur)
                cur = None
        elif cur is not None:
            cur += c
        i += 1
    return (parts + [None, None])[:2]


def read(path_or_bytes):
    if isinstance(path_or_bytes, (bytes, bytearray)):
        data = path_or_bytes
    else:
        with open(path_or_bytes, "rb") as fh:
            data = fh.read()
    out = {"props": {}, "ptypes": {}, "groups": {}}
    index = {}          # object path -> (dtype, count, total_bytes) of its last raw data index
    active = []         # object paths that carry raw data, in segment order
    pos = 0
    while pos + 28 <= len(data):
        if data[pos:pos + 4] != b"TDSm":
            raise TdmsError(f"bad segment tag at {pos}")
        toc, _version, next_off, raw_off = struct.unpack_from("<IIQQ", data, pos + 4)
        if toc & (TOC_BIG_ENDIAN | TOC_INTERLEAVED | TOC_DAQMX):
            raise TdmsError("big-endian, interleaved and DAQmx TDMS data are not supported")
        body = pos + 28
        end = len(data) if next_off == 0xFFFFFFFFFFFFFFFF else body + next_off
        if toc & TOC_NEW_LIST:
            active = []
        if toc & TOC_META:
            r = _Reader(data, body)
            for _ in range(r.take("I")):
                opath = r.string()
                idx = r.take("I")
                if idx == 0xFFFFFFFF:                      # no data in this segment
                    if opath in active:
                        active.remove(opath)
                elif idx == 0:                             # same index as before
                    if opath not in active:
                        active.append(opath)
                else:
                    dtype, dim, count = r.take("IIQ")
                    total = r.take("Q") if dtype == _STRING else count * struct.calcsize("<" + _FIXED.get(dtype, "B"))
                    if dim != 1:
                        raise TdmsError("only 1-D TDMS channels are supported")
                    index[opath] = (dtype, count, total)
                    if opath not in active:
                        active.append(opath)
                props, ptypes = {}, {}
                for _ in range(r.take("I")):
                    name = r.string()
                    ptypes[name] = r.take("I")
                    props[name] = r.value(ptypes[name])
                group, chan = _split_path(opath)
                if group is None:
                    out["props"].update(props)
                    out["ptypes"].update(ptypes)
                    continue
                g = out["groups"].setdefault(group, {"props": {}, "ptypes": {}, "channels": {}})
                if chan is None:
                    g["props"].update(props)
                    g["ptypes"].update(ptypes)
                else:
                    c = g["channels"].setdefault(chan, [[], {}, [], {}, None])
                    c[1].update(props)
                    c[3].update(ptypes)
                    if opath in index:
                        c[4] = index[opath][0]
        if toc & TOC_RAW:
            rp = body + raw_off
            while rp < end:
                start = rp
                for opath in active:
                    dtype, count, total = index[opath]
                    group, chan = _split_path(opath)
                    vals = _read_values(data, rp, dtype, count, total)
                    rp += total
                    if group is not None and chan is not None:
                        c = out["groups"].setdefault(group, {"props": {}, "ptypes": {}, "channels": {}})["channels"] \
                            .setdefault(chan, [[], {}, [], {}, dtype])
                        c[0].extend(vals)
                        c[2].append((rp - total, count, dtype))
                if rp == start:
                    break
        pos = end
    return out


def _read_values(data, pos, dtype, count, total):
    if count == 0:
        return []
    if dtype == _STRING:
        ends = struct.unpack_from(f"<{count}I", data, pos)
        base, prev, vals = pos + 4 * count, 0, []
        for e in ends:
            vals.append(_decode(data[base + prev:base + e]))
            prev = e
        return vals
    if dtype == _TIMESTAMP:
        raw = struct.unpack_from(f"<{2 * count}Q", data, pos)
        return [struct.unpack("<q", struct.pack("<Q", raw[2 * i + 1]))[0] + raw[2 * i] / 2.0 ** 64 for i in range(count)]
    if dtype not in _FIXED:
        raise TdmsError(f"unsupported TDMS data type 0x{dtype:X}")
    return list(struct.unpack_from(f"<{count}{_FIXED[dtype]}", data, pos))


def channel(tdms, group, name, default=None):
    g = tdms["groups"].get(group)
    if not g or name not in g["channels"]:
        return [] if default is None else default
    return g["channels"][name][0]


def patch_u32(data, tdms, group, name, index, value):
    """Return a copy of the file bytes with element `index` of a u32 channel set to `value`."""
    for offset, count, dtype in tdms["groups"][group]["channels"][name][2]:
        if index < count:
            if dtype != 7:
                raise TdmsError(f"{group}/{name} is not a u32 channel")
            out = bytearray(data)
            struct.pack_into("<I", out, offset + 4 * index, value & 0xFFFFFFFF)
            return bytes(out)
        index -= count
    raise IndexError(f"{group}/{name}[{index}] out of range")


# --------------------------------------------------------------------------- writing

def _string(s):
    b = s.encode("utf-8")
    return struct.pack("<I", len(b)) + b


def _prop(name, dtype, value):
    out = _string(name) + struct.pack("<I", dtype)
    if dtype == _STRING:
        return out + _string(value)
    if dtype == _TIMESTAMP:
        secs = int(value)
        return out + struct.pack("<Qq", int((value - secs) * 2.0 ** 64), secs)
    return out + struct.pack("<" + _FIXED[dtype], value)


def _path(group=None, chan=None):
    q = lambda s: "'" + s.replace("'", "''") + "'"
    return "/" if group is None else "/" + q(group) + ("" if chan is None else "/" + q(chan))


def _segment(path, props, ptypes, dtype=None, values=None):
    meta = struct.pack("<I", 1) + _string(path)
    raw = b""
    if dtype is None:
        meta += struct.pack("<I", 0xFFFFFFFF)
        toc = TOC_META | TOC_NEW_LIST
    else:
        if dtype == _STRING:
            blobs = [v.encode("utf-8") for v in values]
            ends, total = [], 0
            for b in blobs:
                total += len(b)
                ends.append(total)
            raw = struct.pack(f"<{len(ends)}I", *ends) + b"".join(blobs)
            meta += struct.pack("<IIIQQ", 28, dtype, 1, len(values), len(raw))
        else:
            raw = struct.pack(f"<{len(values)}{_FIXED[dtype]}", *values)
            meta += struct.pack("<IIIQ", 20, dtype, 1, len(values))
        toc = TOC_META | TOC_NEW_LIST | TOC_RAW
    meta += struct.pack("<I", len(props)) + b"".join(_prop(k, ptypes.get(k, _STRING), v) for k, v in props.items())
    return b"TDSm" + struct.pack("<IIQQ", toc, 4713, len(meta) + len(raw), len(meta)) + meta + raw


def write(tdms):
    """Serialise a structure shaped like read()'s result (one segment per object)."""
    out = [_segment("/", tdms["props"], tdms.get("ptypes", {}))]
    for gname, g in tdms["groups"].items():
        out.append(_segment(_path(gname), g["props"], g.get("ptypes", {})))
        for cname, c in g["channels"].items():
            values, props = c[0], c[1]
            ptypes = c[3] if len(c) > 3 else {}
            dtype = c[4] if len(c) > 4 and c[4] is not None else 5
            out.append(_segment(_path(gname, cname), props, ptypes, dtype, values))
    return b"".join(out)
