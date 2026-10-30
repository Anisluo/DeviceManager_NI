#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace tdc {

// =========================================================================== XML

std::string XmlNode::attr(const std::string& name, const std::string& def) const
{
    for (auto& a : attrs) if (a.first == name) return a.second;
    return def;
}

const XmlNode* XmlNode::child(const std::string& t) const
{
    for (auto& c : children) if (c.tag == t) return &c;
    return nullptr;
}

std::string XmlNode::child_text(const std::string& t, const std::string& def) const
{
    const XmlNode* c = child(t);
    return c ? c->text : def;
}

void XmlNode::walk(const std::function<void(const XmlNode&)>& fn) const
{
    fn(*this);
    for (auto& c : children) c.walk(fn);
}

namespace {

std::string local(const std::string& name)
{
    const size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

void utf8(std::string& out, unsigned long cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

std::string unescape(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#')
            utf8(out, std::strtoul(ent.c_str() + (ent.size() > 1 && ent[1] == 'x' ? 2 : 1), nullptr, ent.size() > 1 && ent[1] == 'x' ? 16 : 10));
        else { out += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

struct XmlParser {
    const std::string& s;
    size_t p = 0;

    [[noreturn]] void fail(const std::string& what) { throw std::runtime_error("XML: " + what + " at " + std::to_string(p)); }
    void skip_ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    bool starts(const char* lit) const { return s.compare(p, std::strlen(lit), lit) == 0; }

    void skip_misc()
    {
        for (;;) {
            skip_ws();
            if (starts("<?")) { const size_t e = s.find("?>", p); if (e == std::string::npos) fail("unterminated <?"); p = e + 2; }
            else if (starts("<!--")) { const size_t e = s.find("-->", p); if (e == std::string::npos) fail("unterminated comment"); p = e + 3; }
            else if (starts("<!")) { const size_t e = s.find('>', p); if (e == std::string::npos) fail("unterminated <!"); p = e + 1; }
            else return;
        }
    }

    std::string name()
    {
        const size_t a = p;
        while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) && s[p] != '>' && s[p] != '/' && s[p] != '=') ++p;
        if (a == p) fail("expected a name");
        return s.substr(a, p - a);
    }

    XmlNode element()
    {
        if (p >= s.size() || s[p] != '<') fail("expected <");
        ++p;
        XmlNode n;
        n.tag = local(name());
        for (;;) {
            skip_ws();
            if (p >= s.size()) fail("unterminated tag");
            if (s[p] == '/') { if (!starts("/>")) fail("expected />"); p += 2; return n; }
            if (s[p] == '>') { ++p; break; }
            const std::string an = name();
            skip_ws();
            if (p >= s.size() || s[p] != '=') fail("expected =");
            ++p;
            skip_ws();
            const char q = s[p];
            if (q != '"' && q != '\'') fail("expected quote");
            const size_t e = s.find(q, p + 1);
            if (e == std::string::npos) fail("unterminated attribute");
            n.attrs.emplace_back(local(an), unescape(s.substr(p + 1, e - p - 1)));
            p = e + 1;
        }
        std::string text;
        for (;;) {
            if (p >= s.size()) fail("unterminated element " + n.tag);
            if (starts("</")) {
                const size_t e = s.find('>', p);
                if (e == std::string::npos) fail("unterminated end tag");
                p = e + 1;
                n.text = trim(unescape(text));
                return n;
            }
            if (starts("<!--")) { const size_t e = s.find("-->", p); if (e == std::string::npos) fail("unterminated comment"); p = e + 3; continue; }
            if (starts("<![CDATA[")) { const size_t e = s.find("]]>", p); if (e == std::string::npos) fail("unterminated CDATA"); text += s.substr(p + 9, e - p - 9); p = e + 3; continue; }
            if (s[p] == '<') { n.children.push_back(element()); continue; }
            const size_t e = s.find('<', p);
            text += s.substr(p, (e == std::string::npos ? s.size() : e) - p);
            p = e == std::string::npos ? s.size() : e;
        }
    }
};

} // namespace

XmlNode xml_parse(const std::string& text)
{
    XmlParser x{text};
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) x.p = 3;
    x.skip_misc();
    return x.element();
}

std::string xml_escape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '&': out += "&amp;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// =========================================================================== quantities

namespace {

bool is_ident(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80); }

// Number at s[p] with an optional SI prefix and unit; advances p. False if no number there.
bool number_with_unit(const std::string& s, size_t& p, double& value, std::string* unit_text)
{
    const char* begin = s.c_str() + p;
    char* stop = nullptr;
    const double v = std::strtod(begin, &stop);
    if (stop == begin) return false;
    // strtod would also eat "inf"/"nan"/hex; only accept plain decimals
    for (const char* c = begin; c < stop; ++c)
        if (!(std::isdigit(static_cast<unsigned char>(*c)) || *c == '.' || *c == 'e' || *c == 'E' || *c == '+' || *c == '-')) return false;
    size_t q = p + size_t(stop - begin);
    const size_t after_number = q;
    while (q < s.size() && s[q] == ' ') ++q;
    static const std::vector<std::pair<std::string, double>> prefixes = {
        {"f", 1e-15}, {"p", 1e-12}, {"n", 1e-9}, {"u", 1e-6}, {"\xC2\xB5", 1e-6}, {"\xCE\xBC", 1e-6},
        {"m", 1e-3}, {"k", 1e3}, {"M", 1e6}, {"G", 1e9}};
    static const std::vector<std::string> units = {"Hz", "s", "V", "A", "\xCE\xA9"};
    auto try_suffix = [&](const std::string& pre, double scale) -> bool {
        size_t r = q;
        if (!pre.empty()) { if (s.compare(r, pre.size(), pre) != 0) return false; r += pre.size(); }
        std::string u;
        for (auto& cand : units)
            if (s.compare(r, cand.size(), cand) == 0) { u = cand; r += cand.size(); break; }
        if (pre.empty() && u.empty()) return false;
        if (r < s.size() && is_ident(s[r])) return false;            // part of a longer word
        value = v * scale;
        if (unit_text) *unit_text = pre + u;
        p = r;
        return true;
    };
    for (auto& pr : prefixes) if (try_suffix(pr.first, pr.second)) return true;
    if (try_suffix("", 1.0)) return true;
    value = v;
    if (unit_text) unit_text->clear();
    p = after_number;
    return true;
}

struct Expr {
    const std::string& s;
    const std::map<std::string, double>& sym;
    size_t p = 0;
    [[noreturn]] void fail() { throw std::invalid_argument("cannot evaluate '" + s + "'"); }
    void ws() { while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p; }
    double primary()
    {
        ws();
        if (p >= s.size()) fail();
        if (s[p] == '(') { ++p; const double v = sum(); ws(); if (p >= s.size() || s[p] != ')') fail(); ++p; return v; }
        if (s[p] == '-') { ++p; return -primary(); }
        if (s[p] == '+') { ++p; return primary(); }
        if (is_ident(s[p])) {
            const size_t a = p;
            while (p < s.size() && (is_ident(s[p]) || std::isdigit(static_cast<unsigned char>(s[p])) || s[p] == '.')) ++p;
            auto it = sym.find(s.substr(a, p - a));
            if (it == sym.end()) fail();
            return it->second;
        }
        double v = 0;
        if (!number_with_unit(s, p, v, nullptr)) fail();
        return v;
    }
    double product()
    {
        double v = primary();
        for (;;) {
            ws();
            if (p < s.size() && s[p] == '*') { ++p; v *= primary(); }
            else if (p < s.size() && s[p] == '/') { ++p; v /= primary(); }
            else return v;
        }
    }
    double sum()
    {
        double v = product();
        for (;;) {
            ws();
            if (p < s.size() && s[p] == '+') { ++p; v += product(); }
            else if (p < s.size() && s[p] == '-') { ++p; v -= product(); }
            else return v;
        }
    }
};

} // namespace

bool parse_quantity(const std::string& text, double& value)
{
    const std::string t = trim(text);
    size_t p = 0;
    double v = 0;
    if (!number_with_unit(t, p, v, nullptr)) return false;
    while (p < t.size() && t[p] == ' ') ++p;
    if (p != t.size()) return false;
    value = v;
    return true;
}

bool split_quantity(const std::string& text, double& value, std::string& unit)
{
    const std::string t = trim(text);
    size_t p = 0;
    double scaled = 0;
    std::string u;
    if (!number_with_unit(t, p, scaled, &u)) return false;
    while (p < t.size() && t[p] == ' ') ++p;
    if (p != t.size()) return false;
    value = std::strtod(t.c_str(), nullptr);          // the number as written, unit kept apart
    unit = u;
    return true;
}

double evaluate(const std::string& expr, const std::map<std::string, double>& symbols)
{
    Expr e{expr, symbols};
    const double v = e.sum();
    e.ws();
    if (e.p != expr.size()) e.fail();
    return v;
}

std::string fmt_g(double v, int digits)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", digits, v);
    return buf;
}

std::string fmt_time(double seconds, int digits)
{
    if (!std::isfinite(seconds)) return "\xE2\x80\x94";                 // —
    const double a = std::fabs(seconds);
    if (a >= 86400) return fmt_g(seconds / 86400, 4) + " \xE5\xA4\xA9";               // 天
    if (a >= 3600) return fmt_g(seconds / 3600, 4) + " \xE5\xB0\x8F\xE6\x97\xB6";     // 小时
    if (a >= 60) return fmt_g(seconds / 60, 4) + " \xE5\x88\x86\xE9\x92\x9F";         // 分钟
    struct U { const char* name; double scale; };
    static const U units[] = {{"s", 1}, {"ms", 1e-3}, {"\xC2\xB5s", 1e-6}, {"ns", 1e-9}};
    for (const U& u : units)
        if (a >= u.scale * (1 - 1e-9) || u.scale == 1e-9) return fmt_g(seconds / u.scale, digits) + " " + u.name;
    return fmt_g(seconds, digits) + " s";
}

// =========================================================================== files

static fs::path P(const std::string& s) { return fs::u8path(s); }

std::string read_file(const std::string& path)
{
    std::ifstream in(P(path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const std::string& path, const std::string& data)
{
    std::ofstream out(P(path), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path);
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) throw std::runtime_error("write failed: " + path);
}

bool file_exists(const std::string& path)
{
    std::error_code ec;
    return fs::exists(P(path), ec);
}

double file_mtime(const std::string& path)
{
    std::error_code ec;
    const auto t = fs::last_write_time(P(path), ec);
    return ec ? 0.0 : double(t.time_since_epoch().count());
}

std::string path_join(const std::string& a, const std::string& b)
{
    return (P(a) / P(b)).u8string();
}

std::string base_name(const std::string& path) { return P(path).filename().u8string(); }
std::string stem(const std::string& path) { return P(path).stem().u8string(); }
std::string parent_dir(const std::string& path) { return P(path).parent_path().u8string(); }

void make_dirs(const std::string& path)
{
    std::error_code ec;
    fs::create_directories(P(path), ec);
}

std::vector<std::string> find_files(const std::string& root, const std::string& ext)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(P(root), ec)) return out;
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    const std::string want = lower(ext);
    for (auto it = fs::recursive_directory_iterator(P(root), ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string name = lower(it->path().filename().u8string());
        if (name.size() >= want.size() && name.compare(name.size() - want.size(), want.size(), want) == 0)
            out.push_back(it->path().u8string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace tdc
