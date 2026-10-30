// util.hpp — small XML reader, SI quantities / formulas, file helpers for the TDC task.
#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tdc {

// ---- XML (enough for the editor's .pinmap / .specs / .digilevels / .digitiming) ----
struct XmlNode {
    std::string tag;                                         // local name, prefix removed
    std::vector<std::pair<std::string, std::string>> attrs;  // local names
    std::string text;                                        // character data (trimmed)
    std::vector<XmlNode> children;

    std::string attr(const std::string& name, const std::string& def = {}) const;
    const XmlNode* child(const std::string& tag) const;
    std::string child_text(const std::string& tag, const std::string& def = {}) const;
    // Depth-first visit of this node and all descendants.
    void walk(const std::function<void(const XmlNode&)>& fn) const;
};

XmlNode xml_parse(const std::string& text);
std::string xml_escape(const std::string& s);

// ---- quantities ----
// "1 µs" -> 1e-6, "15 mA" -> 0.015, "5" -> 5; false when `text` is not a plain quantity.
bool parse_quantity(const std::string& text, double& value);
// "3.3 V" -> (3.3, "V"), "10 mA" -> (10, "mA"); false when not a plain quantity.
bool split_quantity(const std::string& text, double& value, std::string& unit);
// Formula with + - * / ( ), quantities and symbols such as DC.VDD (SI base units).
double evaluate(const std::string& expr, const std::map<std::string, double>& symbols);
// "1.37066 ms", "1.371 分钟", "50.4 天"; "—" for NaN.
std::string fmt_time(double seconds, int digits = 6);
std::string fmt_g(double v, int digits = 6);

// ---- files (UTF-8 paths) ----
std::string read_file(const std::string& path);
void write_file(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);
double file_mtime(const std::string& path);
std::string path_join(const std::string& a, const std::string& b);
std::string base_name(const std::string& path);              // "a/b/c.ext" -> "c.ext"
std::string stem(const std::string& path);                   // "a/b/c.ext" -> "c"
std::string parent_dir(const std::string& path);
void make_dirs(const std::string& path);
// All files under `root` (recursive) whose name ends with `ext` (case-insensitive), sorted.
std::vector<std::string> find_files(const std::string& root, const std::string& ext);

} // namespace tdc
