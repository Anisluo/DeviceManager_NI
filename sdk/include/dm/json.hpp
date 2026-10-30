// dm/json.hpp — tiny dependency-free JSON value for plugin authors.
//
// Deliberately small: enough to parse args_json and build results. Objects keep
// insertion order. Plugins are free to use nlohmann/json or anything else
// instead; the ABI only sees strings.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dm {

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array  = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : t_(Type::Bool), b_(b) {}
    Json(int v) : t_(Type::Number), n_(v) {}
    Json(long v) : t_(Type::Number), n_(double(v)) {}
    Json(long long v) : t_(Type::Number), n_(double(v)) {}
    Json(unsigned v) : t_(Type::Number), n_(v) {}
    Json(unsigned long long v) : t_(Type::Number), n_(double(v)) {}
    Json(double v) : t_(Type::Number), n_(v) {}
    Json(const char* s) : t_(Type::String), s_(s ? s : "") {}
    Json(std::string s) : t_(Type::String), s_(std::move(s)) {}
    // Object literal: Json{{"a", 1}, {"b", "x"}}
    Json(std::initializer_list<Member> members) : t_(Type::Object), o_(members) {}

    static Json array(std::initializer_list<Json> items = {}) {
        Json j; j.t_ = Type::Array; j.a_ = items; return j;
    }
    static Json object() { Json j; j.t_ = Type::Object; return j; }

    Type type() const { return t_; }
    bool is_null()   const { return t_ == Type::Null; }
    bool is_bool()   const { return t_ == Type::Bool; }
    bool is_number() const { return t_ == Type::Number; }
    bool is_string() const { return t_ == Type::String; }
    bool is_array()  const { return t_ == Type::Array; }
    bool is_object() const { return t_ == Type::Object; }

    double             as_number(double def = 0) const { return t_ == Type::Number ? n_ : (t_ == Type::Bool ? (b_ ? 1 : 0) : def); }
    int                as_int(int def = 0) const       { return t_ == Type::Number ? int(std::lround(n_)) : def; }
    bool               as_bool(bool def = false) const { return t_ == Type::Bool ? b_ : (t_ == Type::Number ? n_ != 0 : def); }
    const std::string& as_string() const { static const std::string e; return t_ == Type::String ? s_ : e; }
    const Array&       items() const     { static const Array e; return t_ == Type::Array ? a_ : e; }
    const Object&      members() const   { static const Object e; return t_ == Type::Object ? o_ : e; }

    size_t size() const { return t_ == Type::Array ? a_.size() : t_ == Type::Object ? o_.size() : 0; }

    // ---- object access ----
    const Json* find(const std::string& key) const {
        if (t_ != Type::Object) return nullptr;
        for (auto& m : o_) if (m.first == key) return &m.second;
        return nullptr;
    }
    bool contains(const std::string& key) const { return find(key) != nullptr; }
    const Json& operator[](const std::string& key) const {
        static const Json null_;
        const Json* j = find(key);
        return j ? *j : null_;
    }
    Json& operator[](const std::string& key) {
        if (t_ == Type::Null) t_ = Type::Object;
        if (t_ != Type::Object) throw JsonError("not an object");
        for (auto& m : o_) if (m.first == key) return m.second;
        o_.emplace_back(key, Json());
        return o_.back().second;
    }
    double      number(const std::string& k, double def) const { const Json* j = find(k); return j ? j->as_number(def) : def; }
    int         integer(const std::string& k, int def) const   { const Json* j = find(k); return j ? j->as_int(def) : def; }
    bool        boolean(const std::string& k, bool def) const  { const Json* j = find(k); return j ? j->as_bool(def) : def; }
    std::string str(const std::string& k, const std::string& def = {}) const {
        const Json* j = find(k); return (j && j->is_string()) ? j->s_ : def;
    }

    // ---- array access ----
    const Json& operator[](size_t i) const { static const Json null_; return (t_ == Type::Array && i < a_.size()) ? a_[i] : null_; }
    // j[0] would otherwise also match the string key overload (0 is a null pointer constant)
    const Json& operator[](int i) const { static const Json null_; return i >= 0 ? (*this)[size_t(i)] : null_; }
    Json& push_back(Json v) {
        if (t_ == Type::Null) t_ = Type::Array;
        if (t_ != Type::Array) throw JsonError("not an array");
        a_.push_back(std::move(v));
        return a_.back();
    }

    // ---- serialisation ----
    std::string dump() const { std::string out; dump_to(out); return out; }

    static Json parse(const std::string& text) {
        Parser p{text.data(), text.data() + text.size()};
        p.ws();
        Json j = p.value(0);
        p.ws();
        if (p.cur != p.end) p.fail("trailing characters");
        return j;
    }

private:
    Type        t_ = Type::Null;
    bool        b_ = false;
    double      n_ = 0;
    std::string s_;
    Array       a_;
    Object      o_;

    static void escape(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += char(c);
            }
        }
        out += '"';
    }

    void dump_to(std::string& out) const {
        switch (t_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += b_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(n_)) { out += "null"; break; }
            char buf[32];
            if (n_ == std::floor(n_) && std::fabs(n_) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", n_);
            else std::snprintf(buf, sizeof buf, "%.15g", n_);
            out += buf;
            break;
        }
        case Type::String: escape(out, s_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < a_.size(); ++i) { if (i) out += ','; a_[i].dump_to(out); }
            out += ']';
            break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < o_.size(); ++i) {
                if (i) out += ',';
                escape(out, o_[i].first);
                out += ':';
                o_[i].second.dump_to(out);
            }
            out += '}';
            break;
        }
    }

    struct Parser {
        const char* cur;
        const char* end;

        [[noreturn]] void fail(const char* what) { throw JsonError(std::string("JSON parse error: ") + what); }
        void ws() { while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r')) ++cur; }
        bool eat(const char* lit) {
            const char* p = cur;
            for (; *lit; ++lit, ++p) if (p >= end || *p != *lit) return false;
            cur = p; return true;
        }

        Json value(int depth) {
            if (depth > 256) fail("nesting too deep");
            if (cur >= end) fail("unexpected end");
            switch (*cur) {
            case '{': return object(depth);
            case '[': return arr(depth);
            case '"': return Json(string());
            case 't': if (eat("true"))  return Json(true);  fail("bad literal");
            case 'f': if (eat("false")) return Json(false); fail("bad literal");
            case 'n': if (eat("null"))  return Json();      fail("bad literal");
            default:  return number();
            }
        }
        Json object(int depth) {
            ++cur; Json j = Json::object(); ws();
            if (cur < end && *cur == '}') { ++cur; return j; }
            for (;;) {
                ws(); if (cur >= end || *cur != '"') fail("expected key");
                std::string k = string(); ws();
                if (cur >= end || *cur != ':') fail("expected ':'");
                ++cur; ws();
                j.o_.emplace_back(std::move(k), value(depth + 1)); ws();
                if (cur < end && *cur == ',') { ++cur; continue; }
                if (cur < end && *cur == '}') { ++cur; return j; }
                fail("expected ',' or '}'");
            }
        }
        Json arr(int depth) {
            ++cur; Json j = Json::array(); ws();
            if (cur < end && *cur == ']') { ++cur; return j; }
            for (;;) {
                ws(); j.a_.push_back(value(depth + 1)); ws();
                if (cur < end && *cur == ',') { ++cur; continue; }
                if (cur < end && *cur == ']') { ++cur; return j; }
                fail("expected ',' or ']'");
            }
        }
        Json number() {
            const char* start = cur;
            if (cur < end && (*cur == '-' || *cur == '+')) ++cur;
            while (cur < end && ((*cur >= '0' && *cur <= '9') || *cur == '.' || *cur == 'e' || *cur == 'E' || *cur == '-' || *cur == '+')) ++cur;
            if (cur == start) fail("unexpected character");
            std::string tok(start, cur);
            char* stop = nullptr;
            double v = std::strtod(tok.c_str(), &stop);
            if (!stop || *stop) fail("bad number");
            return Json(v);
        }
        static void utf8(std::string& out, uint32_t cp) {
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
            else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        }
        uint32_t hex4() {
            if (end - cur < 4) fail("bad \\u escape");
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i, ++cur) {
                char c = *cur; v <<= 4;
                if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
                else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
                else fail("bad \\u escape");
            }
            return v;
        }
        std::string string() {
            ++cur; std::string out;
            while (cur < end && *cur != '"') {
                char c = *cur++;
                if (c != '\\') { out += c; continue; }
                if (cur >= end) fail("bad escape");
                switch (char e = *cur++) {
                case '"': out += '"'; break;  case '\\': out += '\\'; break;
                case '/': out += '/'; break;  case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - cur >= 6 && cur[0] == '\\' && cur[1] == 'u') {
                        cur += 2; uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp); break;
                }
                default: (void)e; fail("bad escape");
                }
            }
            if (cur >= end) fail("unterminated string");
            ++cur;
            return out;
        }
    };
};

} // namespace dm
