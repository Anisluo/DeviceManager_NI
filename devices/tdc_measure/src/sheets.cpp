#include "sheets.hpp"
#include "util.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace tdc {

namespace {

constexpr size_t npos = std::string::npos;

struct Elem {
    size_t begin = npos, inner = 0, inner_end = 0, end = 0;   // <tag>inner</tag>; end is past the close tag
    bool self_closed = false;
    explicit operator bool() const { return begin != npos; }
};

// First element <tag ...> in [from, to), with attr="value" when `attr` is given.
Elem find_elem(const std::string& x, const std::string& tag, size_t from, size_t to, const std::string& attr = {},
               const std::string& value = {})
{
    const std::string open = "<" + tag;
    for (size_t at = x.find(open, from); at != npos && at < to; at = x.find(open, at + 1)) {
        const char c = at + open.size() < x.size() ? x[at + open.size()] : '\0';
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '>' && c != '/') continue;   // <Pins> vs <PinGroups>
        const size_t gt = x.find('>', at);
        if (gt == npos) break;
        if (!attr.empty() && x.substr(at, gt - at).find(" " + attr + "=\"" + xml_escape(value) + "\"") == npos) continue;
        Elem e;
        e.begin = at;
        if (x[gt - 1] == '/') {
            e.inner = e.inner_end = gt;
            e.end = gt + 1;
            e.self_closed = true;
            return e;
        }
        const std::string close = "</" + tag + ">";
        const size_t ce = x.find(close, gt);
        if (ce == npos) break;
        e.inner = gt + 1;
        e.inner_end = ce;
        e.end = ce + close.size();
        return e;
    }
    return {};
}

Elem need(const Elem& e, const std::string& what)
{
    if (!e) throw std::invalid_argument("没有找到 " + what);
    return e;
}

std::string eol_of(const std::string& x) { return x.find("\r\n") != npos ? "\r\n" : "\n"; }

// Start of the indentation of the line holding `pos`.
size_t line_begin(const std::string& x, size_t pos)
{
    while (pos > 0 && (x[pos - 1] == ' ' || x[pos - 1] == '\t')) --pos;
    return pos;
}

std::string indent_at(const std::string& x, size_t pos) { const size_t b = line_begin(x, pos); return x.substr(b, pos - b); }

void erase_lines(std::string& x, const Elem& e)
{
    const size_t b = line_begin(x, e.begin);
    size_t a = e.end;
    if (a < x.size() && x[a] == '\r') ++a;
    if (a < x.size() && x[a] == '\n') ++a;
    x.erase(b, a - b);
}

// New lines before the close tag of `list` (each of `lines` is complete, with its indentation).
void insert_before_close(std::string& x, const Elem& list, const std::string& lines)
{
    x.insert(line_begin(x, list.inner_end), lines);
}

std::string xml_unescape(std::string s)
{
    for (auto& [from, to] : std::vector<std::pair<std::string, std::string>>{{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&amp;", "&"}}) {
        for (size_t at = s.find(from); at != npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    }
    return s;
}

void set_inner(std::string& x, const Elem& e, const std::string& text)
{
    x.replace(e.inner, e.inner_end - e.inner, xml_escape(text));
}

void set_attr(std::string& x, const Elem& e, const std::string& name, const std::string& value)
{
    const size_t gt = x.find('>', e.begin);
    const std::string key = " " + name + "=\"";
    const size_t at = x.find(key, e.begin);
    if (at == npos || at > gt) throw std::invalid_argument("没有属性 " + name);
    const size_t a = at + key.size(), b = x.find('"', a);
    x.replace(a, b - a, xml_escape(value));
}

const std::vector<std::pair<std::string, std::string>>& drive_tags()
{
    // DriveReturnLow: sheets of plugin 0.2
    static const std::vector<std::pair<std::string, std::string>> tags{
        {"NR", "DriveNonReturn"}, {"RL", "ReturnToLow"}, {"RH", "ReturnToHigh"}, {"SBC", "SurroundByComplement"}, {"RL", "DriveReturnLow"}};
    return tags;
}

Elem drive_elem(const std::string& x, const Elem& block)
{
    for (auto& [fmt, tag] : drive_tags()) {
        (void)fmt;
        if (Elem d = find_elem(x, tag, block.inner, block.inner_end)) return d;
    }
    return {};
}

Elem find_timeset(const std::string& x, const std::string& name)
{
    return need(find_elem(x, "TimeSet", 0, x.size(), "name", name), "时序集 " + name);
}

Elem find_pin_edge(const std::string& x, const Elem& ts, const std::string& pin)
{
    return find_elem(x, "PinEdge", ts.inner, ts.inner_end, "pin", pin);
}

std::string drive_text(const std::string& format, const std::map<std::string, std::string>& edges, const std::string& indent,
                       const std::string& eol)
{
    std::string tag;
    for (auto& [fmt, t] : drive_tags()) if (fmt == format && tag.empty()) tag = t;
    if (tag.empty()) throw std::invalid_argument("未知的驱动格式 " + format);
    std::string out = "<" + tag + ">" + eol;
    for (const char* key : {"On", "Data", "Return", "Off"}) {        // element and edge order as the editor saves them
        if (format == "NR" && std::string(key) == "Return") continue;
        out += indent + "  <" + key + ">" + xml_escape(edges.at(key)) + "</" + key + ">" + eol;
    }
    return out + indent + "</" + tag + ">";
}

} // namespace

// =========================================================================== timing

void timing_set_period(std::string& x, const std::string& timeset, const std::string& text)
{
    const Elem ts = find_timeset(x, timeset);
    set_inner(x, need(find_elem(x, "Period", ts.inner, ts.inner_end), timeset + " 的周期"), text);
}

void timing_set_edge(std::string& x, const std::string& timeset, const std::string& pin, const std::string& key,
                     const std::string& text)
{
    const Elem ts = find_timeset(x, timeset);
    const Elem block = need(find_pin_edge(x, ts, pin), timeset + " 里 " + pin + " 的边沿");
    if (key == "Strobe") {
        if (const Elem cs = find_elem(x, "CompareStrobe", block.inner, block.inner_end)) {
            set_inner(x, need(find_elem(x, "Strobe", cs.inner, cs.inner_end), "Strobe"), text);
            return;
        }
        const Elem d = drive_elem(x, block);
        const size_t at = d ? d.end : block.inner;
        const std::string ind = d ? indent_at(x, d.begin) : indent_at(x, block.begin) + "  ";
        const std::string eol = eol_of(x);
        x.insert(at, eol + ind + "<CompareStrobe>" + eol + ind + "  <Strobe>" + xml_escape(text) + "</Strobe>" + eol + ind + "</CompareStrobe>");
        return;
    }
    const Elem d = need(drive_elem(x, block), pin + " 的驱动格式");
    set_inner(x, need(find_elem(x, key, d.inner, d.inner_end), "这个驱动格式没有 " + key + " 边沿，"), text);
}

void timing_set_format(std::string& x, const std::string& timeset, const std::string& pin, const std::string& format,
                       const std::string& default_return)
{
    const Elem ts = find_timeset(x, timeset);
    const Elem block = need(find_pin_edge(x, ts, pin), timeset + " 里 " + pin + " 的边沿");
    std::map<std::string, std::string> edges{{"On", "0"}, {"Data", "0"}, {"Return", default_return}, {"Off", "0"}};
    const std::string eol = eol_of(x);
    if (const Elem d = drive_elem(x, block)) {
        for (auto& [key, text] : edges)
            if (const Elem e = find_elem(x, key, d.inner, d.inner_end)) text = xml_unescape(x.substr(e.inner, e.inner_end - e.inner));
        const std::string ind = indent_at(x, d.begin);
        x.replace(d.begin, d.end - d.begin, drive_text(format, edges, ind, eol));
    } else {
        const std::string ind = indent_at(x, block.begin) + "  ";
        x.insert(block.inner, eol + ind + drive_text(format, edges, ind, eol));
    }
}

void timing_add_pin(std::string& x, const std::string& timeset, const std::string& pin, const std::string& copy_from)
{
    const Elem ts = find_timeset(x, timeset);
    if (find_pin_edge(x, ts, pin)) throw std::invalid_argument(timeset + " 里已有 " + pin);
    const Elem list = need(find_elem(x, "PinEdges", ts.inner, ts.inner_end), timeset + " 的 PinEdges");
    const std::string eol = eol_of(x);
    const Elem from = copy_from.empty() ? Elem{} : find_pin_edge(x, ts, copy_from);
    std::string lines;
    if (from) {
        std::string block = x.substr(from.begin, from.end - from.begin);
        const std::string attr = "pin=\"" + xml_escape(copy_from) + "\"";
        block.replace(block.find(attr), attr.size(), "pin=\"" + xml_escape(pin) + "\"");
        lines = indent_at(x, from.begin) + block + eol;
    } else {
        const std::string ind = indent_at(x, list.inner_end) + "  ", in2 = ind + "  ";
        const std::map<std::string, std::string> zero{{"On", "0"}, {"Data", "0"}, {"Return", "0"}, {"Off", "0"}};
        lines = ind + "<PinEdge pin=\"" + xml_escape(pin) + "\">" + eol +
                in2 + drive_text("NR", zero, in2, eol) + eol +
                in2 + "<CompareStrobe>" + eol + in2 + "  <Strobe>0</Strobe>" + eol + in2 + "</CompareStrobe>" + eol +
                in2 + "<DataSource>Pattern</DataSource>" + eol + ind + "</PinEdge>" + eol;
    }
    insert_before_close(x, list, lines);
}

void timing_remove_pin(std::string& x, const std::string& timeset, const std::string& pin)
{
    const Elem ts = find_timeset(x, timeset);
    erase_lines(x, need(find_pin_edge(x, ts, pin), timeset + " 里 " + pin + " 的边沿"));
}

void timing_add_timeset(std::string& x, const std::string& name, const std::string& copy_of)
{
    if (find_elem(x, "TimeSet", 0, x.size(), "name", name)) throw std::invalid_argument("已有时序集 " + name);
    const Elem from = find_timeset(x, copy_of);
    std::string block = x.substr(from.begin, from.end - from.begin);
    const std::string attr = "name=\"" + xml_escape(copy_of) + "\"";
    block.replace(block.find(attr), attr.size(), "name=\"" + xml_escape(name) + "\"");
    x.insert(from.end, eol_of(x) + indent_at(x, from.begin) + block);
}

void timing_remove_timeset(std::string& x, const std::string& name) { erase_lines(x, find_timeset(x, name)); }

// =========================================================================== levels

static Elem find_level(const std::string& x, const std::string& pin)
{
    return find_elem(x, "DigitalPinLevelSet", 0, x.size(), "pin", pin);
}

void levels_set(std::string& x, const std::string& pin, const std::string& field, const std::string& text)
{
    const Elem block = need(find_level(x, pin), pin + " 的电平");
    if (const Elem e = find_elem(x, field, block.inner, block.inner_end)) {
        set_inner(x, e, text);
        return;
    }
    const std::string ind = indent_at(x, block.begin) + "  ";
    insert_before_close(x, block, ind + "<" + field + ">" + xml_escape(text) + "</" + field + ">" + eol_of(x));
}

void levels_add(std::string& x, const std::string& pin, const std::string& copy_of)
{
    if (find_level(x, pin)) throw std::invalid_argument(pin + " 已有电平");
    const Elem from = need(find_level(x, copy_of), copy_of + " 的电平");
    std::string block = x.substr(from.begin, from.end - from.begin);
    const std::string attr = "pin=\"" + xml_escape(copy_of) + "\"";
    block.replace(block.find(attr), attr.size(), "pin=\"" + xml_escape(pin) + "\"");
    x.insert(from.end, eol_of(x) + indent_at(x, from.begin) + block);
}

void levels_remove(std::string& x, const std::string& pin) { erase_lines(x, need(find_level(x, pin), pin + " 的电平")); }

// =========================================================================== specifications

static Elem find_section(const std::string& x, const std::string& section)
{
    return find_elem(x, "Section", 0, x.size(), "name", section);
}

static Elem find_formula(const std::string& x, const std::string& section, const std::string& symbol)
{
    const Elem s = need(find_section(x, section), "规格分组 " + section);
    return need(find_elem(x, "f:Formula", s.inner, s.inner_end, "symbol", symbol), "规格 " + section + "." + symbol);
}

void specs_set(std::string& x, const std::string& section, const std::string& symbol, const std::string& field,
               const std::string& text)
{
    const Elem f = find_formula(x, section, symbol);
    const std::string tag = "f:" + field;
    if (const Elem e = find_elem(x, tag, f.inner, f.inner_end)) {
        if (e.self_closed) x.replace(e.begin, e.end - e.begin, "<" + tag + ">" + xml_escape(text) + "</" + tag + ">");
        else set_inner(x, e, text);
        return;
    }
    const std::string ind = indent_at(x, f.begin) + "  ";
    insert_before_close(x, f, ind + "<" + tag + ">" + xml_escape(text) + "</" + tag + ">" + eol_of(x));
}

void specs_add(std::string& x, const std::string& section, const std::string& symbol, const std::string& definition)
{
    const std::string eol = eol_of(x);
    Elem s = find_section(x, section);
    if (!s) {
        const Elem root = need(find_elem(x, "Specifications", 0, x.size()), "Specifications");
        insert_before_close(x, root, "  <Section name=\"" + xml_escape(section) + "\">" + eol + "  </Section>" + eol);
        s = find_section(x, section);
    }
    if (find_elem(x, "f:Formula", s.inner, s.inner_end, "symbol", symbol)) throw std::invalid_argument("已有规格 " + section + "." + symbol);
    const std::string ind = indent_at(x, s.begin) + "  ";
    insert_before_close(x, s, ind + "<f:Formula symbol=\"" + xml_escape(symbol) + "\">" + eol + ind + "  <f:Definition>" +
                                  xml_escape(definition) + "</f:Definition>" + eol + ind + "</f:Formula>" + eol);
}

void specs_remove(std::string& x, const std::string& section, const std::string& symbol)
{
    erase_lines(x, find_formula(x, section, symbol));
}

// =========================================================================== pin map
void pinmap_add_pin(std::string& x, const std::string& pin)
{
    if (find_elem(x, "DUTPin", 0, x.size(), "name", pin)) throw std::invalid_argument("已有引脚 " + pin);
    const Elem pins = need(find_elem(x, "Pins", 0, x.size()), "Pins");
    const Elem first = find_elem(x, "DUTPin", pins.inner, pins.inner_end);
    const std::string ind = first ? indent_at(x, first.begin) : indent_at(x, pins.inner_end) + "\t";
    insert_before_close(x, pins, ind + "<DUTPin name=\"" + xml_escape(pin) + "\" />" + eol_of(x));
}

void pinmap_remove_pin(std::string& x, const std::string& pin)
{
    erase_lines(x, need(find_elem(x, "DUTPin", 0, x.size(), "name", pin), "引脚 " + pin));
    while (const Elem e = find_elem(x, "PinReference", 0, x.size(), "pin", pin)) erase_lines(x, e);
    while (const Elem e = find_elem(x, "Connection", 0, x.size(), "pin", pin)) erase_lines(x, e);
}

void pinmap_set_connection(std::string& x, const std::string& pin, const std::string& instrument, int channel)
{
    if (const Elem e = find_elem(x, "Connection", 0, x.size(), "pin", pin)) {
        set_attr(x, e, "instrument", instrument);
        set_attr(x, e, "channel", std::to_string(channel));
        return;
    }
    const Elem list = need(find_elem(x, "Connections", 0, x.size()), "Connections");
    const Elem first = find_elem(x, "Connection", list.inner, list.inner_end);
    const std::string ind = first ? indent_at(x, first.begin) : indent_at(x, list.inner_end) + "\t";
    insert_before_close(x, list, ind + "<Connection pin=\"" + xml_escape(pin) + "\" siteNumber=\"0\" instrument=\"" +
                                     xml_escape(instrument) + "\" channel=\"" + std::to_string(channel) + "\" />" + eol_of(x));
}

void pinmap_set_groups(std::string& x, const std::string& pin, const std::vector<std::string>& groups)
{
    const std::string eol = eol_of(x);
    const std::string ref = "<PinReference pin=\"" + xml_escape(pin) + "\" />";
    std::vector<std::string> names;
    for (Elem g = find_elem(x, "PinGroup", 0, x.size()); g; g = find_elem(x, "PinGroup", g.end, x.size())) {
        const size_t a = x.find("name=\"", g.begin) + 6;
        names.push_back(x.substr(a, x.find('"', a) - a));
    }
    for (auto& name : names) {                                       // names as written (escaped)
        const size_t at = x.find("<PinGroup name=\"" + name + "\"");
        const Elem g = need(find_elem(x, "PinGroup", at, x.size()), "分组");
        const bool want = std::find(groups.begin(), groups.end(), name) != groups.end();
        const Elem has = find_elem(x, "PinReference", g.inner, g.inner_end, "pin", pin);
        if (has && !want) {
            erase_lines(x, has);
        } else if (!has && want) {
            if (g.self_closed) throw std::invalid_argument("分组 " + name + " 是空元素，请先在编辑器里添加引脚");
            const Elem first = find_elem(x, "PinReference", g.inner, g.inner_end);
            const std::string ind = first ? indent_at(x, first.begin) : indent_at(x, g.begin) + "\t";
            insert_before_close(x, g, ind + ref + eol);
        }
    }
    for (auto& name : groups) {
        if (std::find(names.begin(), names.end(), name) != names.end()) continue;
        const Elem list = need(find_elem(x, "PinGroups", 0, x.size()), "PinGroups");
        const std::string ind = indent_at(x, list.inner_end) + "\t";
        insert_before_close(x, list, ind + "<PinGroup name=\"" + xml_escape(name) + "\">" + eol + ind + "\t" + ref + eol + ind + "</PinGroup>" + eol);
    }
}

void pinmap_rename_instrument(std::string& x, const std::string& from, const std::string& to)
{
    set_attr(x, need(find_elem(x, "NIDigitalPatternInstrument", 0, x.size(), "name", from), "仪器 " + from), "name", to);
    while (const Elem e = find_elem(x, "Connection", 0, x.size(), "instrument", from)) set_attr(x, e, "instrument", to);
}

} // namespace tdc
