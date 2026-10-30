#include "tdms.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace tdms {

namespace {

constexpr uint32_t TOC_META = 1u << 1, TOC_NEW_LIST = 1u << 2, TOC_RAW = 1u << 3,
                   TOC_INTERLEAVED = 1u << 5, TOC_BIG_ENDIAN = 1u << 6, TOC_DAQMX = 1u << 7;

size_t type_size(uint32_t t)
{
    switch (t) {
    case I8: case U8: case BOOL: return 1;
    case I16: case U16: return 2;
    case I32: case U32: case SGL: return 4;
    case I64: case U64: case DBL: return 8;
    case TIMESTAMP: return 16;
    default: throw Error("unsupported TDMS type " + std::to_string(t));
    }
}

struct Reader {
    const std::string& d;
    size_t p;
    void need(size_t n) const { if (p + n > d.size()) throw Error("TDMS file truncated"); }
    template <class T> T take() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, d.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string string() {
        const uint32_t n = take<uint32_t>();
        need(n);
        std::string s = d.substr(p, n);
        p += n;
        return s;
    }
    double number(uint32_t t) {
        switch (t) {
        case I8:  return take<int8_t>();
        case U8:  return take<uint8_t>();
        case BOOL: return take<uint8_t>() ? 1 : 0;
        case I16: return take<int16_t>();
        case U16: return take<uint16_t>();
        case I32: return take<int32_t>();
        case U32: return take<uint32_t>();
        case I64: return double(take<int64_t>());
        case U64: return double(take<uint64_t>());
        case SGL: return take<float>();
        case DBL: return take<double>();
        case TIMESTAMP: { const uint64_t frac = take<uint64_t>(); const int64_t secs = take<int64_t>();
                          return double(secs) + double(frac) / 18446744073709551616.0; }
        default: throw Error("unsupported TDMS type " + std::to_string(t));
        }
    }
};

// "/'Group'/'Channel'" -> parts ("" for missing)
void split_path(const std::string& path, std::string& group, std::string& chan, int& depth)
{
    std::vector<std::string> parts;
    std::string cur;
    bool in = false;
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '\'') {
            if (!in) { in = true; cur.clear(); }
            else if (i + 1 < path.size() && path[i + 1] == '\'') { cur += '\''; ++i; }
            else { in = false; parts.push_back(cur); }
        } else if (in) {
            cur += c;
        }
    }
    depth = int(parts.size());
    group = depth > 0 ? parts[0] : "";
    chan = depth > 1 ? parts[1] : "";
}

struct Index { uint32_t type = U8; uint64_t count = 0; uint64_t total = 0; };

void put_u32(std::string& o, uint32_t v) { o.append(reinterpret_cast<const char*>(&v), 4); }
void put_u64(std::string& o, uint64_t v) { o.append(reinterpret_cast<const char*>(&v), 8); }
void put_str(std::string& o, const std::string& s) { put_u32(o, uint32_t(s.size())); o += s; }

void put_num(std::string& o, uint32_t t, double v)
{
    auto raw = [&](auto x) { o.append(reinterpret_cast<const char*>(&x), sizeof(x)); };
    switch (t) {
    case I8:  raw(int8_t(v)); break;
    case U8:  raw(uint8_t(v)); break;
    case BOOL: raw(uint8_t(v != 0)); break;
    case I16: raw(int16_t(v)); break;
    case U16: raw(uint16_t(v)); break;
    case I32: raw(int32_t(v)); break;
    case U32: raw(uint32_t(v)); break;
    case I64: raw(int64_t(v)); break;
    case U64: raw(uint64_t(v)); break;
    case SGL: raw(float(v)); break;
    case DBL: raw(double(v)); break;
    case TIMESTAMP: { const int64_t secs = int64_t(v); raw(uint64_t((v - double(secs)) * 18446744073709551616.0)); raw(secs); break; }
    default: throw Error("unsupported TDMS type " + std::to_string(t));
    }
}

void put_prop(std::string& o, const Prop& p)
{
    put_str(o, p.name);
    put_u32(o, p.type);
    if (p.type == STRING) put_str(o, p.str);
    else put_num(o, p.type, p.num);
}

std::string quote(const std::string& s)
{
    std::string out = "'";
    for (char c : s) { out += c; if (c == '\'') out += '\''; }
    return out + "'";
}

std::string segment(const std::string& path, const std::vector<Prop>& props, const Channel* ch)
{
    std::string meta, raw;
    put_u32(meta, 1);
    put_str(meta, path);
    uint32_t toc = TOC_META | TOC_NEW_LIST;
    if (!ch) {
        put_u32(meta, 0xFFFFFFFFu);
    } else {
        if (ch->type == STRING) {
            uint32_t end = 0;
            std::string blob;
            for (auto& s : ch->strs) { end += uint32_t(s.size()); put_u32(raw, end); blob += s; }
            raw += blob;
            put_u32(meta, 28); put_u32(meta, STRING); put_u32(meta, 1);
            put_u64(meta, ch->strs.size()); put_u64(meta, raw.size());
        } else {
            for (double v : ch->nums) put_num(raw, ch->type, v);
            put_u32(meta, 20); put_u32(meta, ch->type); put_u32(meta, 1); put_u64(meta, ch->nums.size());
        }
        toc |= TOC_RAW;
    }
    put_u32(meta, uint32_t(props.size()));
    for (auto& p : props) put_prop(meta, p);
    std::string out = "TDSm";
    put_u32(out, toc);
    put_u32(out, 4713);
    put_u64(out, meta.size() + raw.size());
    put_u64(out, meta.size());
    return out + meta + raw;
}

} // namespace

Channel* Group::channel(const std::string& n)
{
    for (auto& c : channels) if (c.name == n) return &c;
    return nullptr;
}
const Channel* Group::channel(const std::string& n) const
{
    for (auto& c : channels) if (c.name == n) return &c;
    return nullptr;
}
Group* File::group(const std::string& n)
{
    for (auto& g : groups) if (g.name == n) return &g;
    return nullptr;
}
const Group* File::group(const std::string& n) const
{
    for (auto& g : groups) if (g.name == n) return &g;
    return nullptr;
}
const Channel* File::channel(const std::string& g, const std::string& c) const
{
    const Group* gr = group(g);
    return gr ? gr->channel(c) : nullptr;
}
Channel* File::channel(const std::string& g, const std::string& c)
{
    Group* gr = group(g);
    return gr ? gr->channel(c) : nullptr;
}

const Prop* find_prop(const std::vector<Prop>& props, const std::string& name)
{
    for (auto& p : props) if (p.name == name) return &p;
    return nullptr;
}

void set_prop(std::vector<Prop>& props, const std::string& name, double num)
{
    for (auto& p : props) if (p.name == name) { p.num = num; return; }
    props.push_back(Prop{name, DBL, num, {}});
}

void set_prop(std::vector<Prop>& props, const std::string& name, const std::string& str)
{
    for (auto& p : props) if (p.name == name) { p.str = str; return; }
    props.push_back(Prop{name, STRING, 0, str});
}

File read(const std::string& d)
{
    File f;
    std::map<std::string, Index> index;
    std::vector<std::string> active;
    auto upsert = [](std::vector<Prop>& props, Prop p) {
        for (auto& q : props) if (q.name == p.name) { q = std::move(p); return; }
        props.push_back(std::move(p));
    };
    auto group_of = [&f](const std::string& name) -> Group& {
        if (Group* g = f.group(name)) return *g;
        f.groups.push_back(Group{name, {}, {}});
        return f.groups.back();
    };
    auto channel_of = [&](const std::string& g, const std::string& c, uint32_t type) -> Channel& {
        Group& gr = group_of(g);
        if (Channel* ch = gr.channel(c)) return *ch;
        Channel ch;
        ch.name = c;
        ch.type = type;
        gr.channels.push_back(ch);
        return gr.channels.back();
    };

    size_t pos = 0;
    while (pos + 28 <= d.size()) {
        if (d.compare(pos, 4, "TDSm") != 0) throw Error("bad TDMS segment tag at " + std::to_string(pos));
        Reader h{d, pos + 4};
        const uint32_t toc = h.take<uint32_t>();
        h.take<uint32_t>();                                   // version
        const uint64_t next = h.take<uint64_t>();
        const uint64_t raw_off = h.take<uint64_t>();
        if (toc & (TOC_BIG_ENDIAN | TOC_INTERLEAVED | TOC_DAQMX))
            throw Error("big-endian, interleaved and DAQmx TDMS data are not supported");
        const size_t body = pos + 28;
        const size_t end = next == 0xFFFFFFFFFFFFFFFFull ? d.size() : size_t(body + next);
        if (toc & TOC_NEW_LIST) active.clear();
        if (toc & TOC_META) {
            Reader r{d, body};
            const uint32_t n = r.take<uint32_t>();
            for (uint32_t k = 0; k < n; ++k) {
                const std::string path = r.string();
                const uint32_t idx = r.take<uint32_t>();
                if (idx == 0xFFFFFFFFu) {
                    active.erase(std::remove(active.begin(), active.end(), path), active.end());
                } else if (idx == 0) {
                    if (std::find(active.begin(), active.end(), path) == active.end()) active.push_back(path);
                } else {
                    Index ix;
                    ix.type = r.take<uint32_t>();
                    if (r.take<uint32_t>() != 1) throw Error("only 1-D TDMS channels are supported");
                    ix.count = r.take<uint64_t>();
                    ix.total = ix.type == STRING ? r.take<uint64_t>() : ix.count * type_size(ix.type);
                    index[path] = ix;
                    if (std::find(active.begin(), active.end(), path) == active.end()) active.push_back(path);
                }
                std::vector<Prop> props;
                const uint32_t np = r.take<uint32_t>();
                for (uint32_t q = 0; q < np; ++q) {
                    Prop p;
                    p.name = r.string();
                    p.type = r.take<uint32_t>();
                    if (p.type == STRING) p.str = r.string();
                    else p.num = r.number(p.type);
                    props.push_back(std::move(p));
                }
                std::string g, c;
                int depth = 0;
                split_path(path, g, c, depth);
                if (depth == 0) {
                    for (auto& p : props) upsert(f.props, p);
                } else if (depth == 1) {
                    Group& gr = group_of(g);
                    for (auto& p : props) upsert(gr.props, p);
                } else {
                    auto it = index.find(path);
                    Channel& ch = channel_of(g, c, it != index.end() ? it->second.type : U8);
                    if (it != index.end()) ch.type = it->second.type;
                    for (auto& p : props) upsert(ch.props, p);
                }
            }
        }
        if (toc & TOC_RAW) {
            size_t rp = body + size_t(raw_off);
            while (rp < end) {
                const size_t start = rp;
                for (const auto& path : active) {
                    const Index& ix = index.at(path);
                    std::string g, c;
                    int depth = 0;
                    split_path(path, g, c, depth);
                    if (rp + ix.total > d.size()) throw Error("TDMS raw data truncated");
                    if (depth == 2) {
                        Channel& ch = channel_of(g, c, ix.type);
                        ch.chunks.push_back(Chunk{rp, size_t(ix.count), ix.type});
                        if (ix.type == STRING) {
                            Reader r{d, rp};
                            std::vector<uint32_t> ends(size_t(ix.count));
                            for (auto& e : ends) e = r.take<uint32_t>();
                            const size_t base = rp + 4 * size_t(ix.count);
                            uint32_t prev = 0;
                            for (uint32_t e : ends) { ch.strs.push_back(d.substr(base + prev, e - prev)); prev = e; }
                        } else {
                            Reader r{d, rp};
                            for (uint64_t k = 0; k < ix.count; ++k) ch.nums.push_back(r.number(ix.type));
                        }
                    }
                    rp += size_t(ix.total);
                }
                if (rp == start) break;
            }
        }
        pos = end;
    }
    return f;
}

std::string write(const File& f)
{
    std::string out = segment("/", f.props, nullptr);
    for (auto& g : f.groups) {
        out += segment("/" + quote(g.name), g.props, nullptr);
        for (auto& c : g.channels) out += segment("/" + quote(g.name) + "/" + quote(c.name), c.props, &c);
    }
    return out;
}

std::string patch_u32(const std::string& bytes, const File& f, const std::string& group,
                      const std::string& channel, size_t index, uint32_t value)
{
    const Channel* ch = f.channel(group, channel);
    if (!ch) throw Error(group + "/" + channel + " not found");
    for (auto& c : ch->chunks) {
        if (index < c.count) {
            if (c.type != U32) throw Error(group + "/" + channel + " is not a u32 channel");
            std::string out = bytes;
            std::memcpy(&out[c.offset + 4 * index], &value, 4);
            return out;
        }
        index -= c.count;
    }
    throw Error(group + "/" + channel + " index out of range");
}

} // namespace tdms
