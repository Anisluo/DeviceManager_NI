// project.hpp — Digital Pattern Editor project model for the TDC task.
//
// Reads a project folder (pin map, specifications, levels, timing, compiled .digipat), decodes
// vectors, runs a pattern through an interpreter (timeline of the first measurement, loops
// extrapolated) and derives the TDC stimulus timing. Repeat / set_loop counts can be edited and
// patched into a copy of the compiled file.
//
// Compiled vector encoding (Digital Pattern Editor 21.8 files, Architecture 1):
//   PinState:    4 bits per pin, pin i in byte i / 2, even pins in the low nibble;
//                values as nidigital.PinState: 0, 1, L=3, H=4, X=5, M=6, V=7, D=8, E=9.
//   Instruction: bits 31..28 opcode (0 next, 1 repeat, 4 set_loop, 5 end_loop, 7 jump_if, F halt),
//                bits 27..23 timeset index, bits 22..16 flags, bits 15..0 operand (count or label
//                index; bit 15 of a jump_if operand is a flag).
#pragma once

#include "tdms.hpp"

#include <dm/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace tdc {

constexpr int COUNT_MAX = 0xFFFF;          // repeat / set_loop counts are 16-bit in this encoding

struct Decoded {
    unsigned op = 0, timeset = 0, flags = 0, operand = 0;
};
Decoded decode(uint32_t ins);
std::string op_name(unsigned op);

struct Timeset {
    std::string period_text;
    double period = 0;                     // 0 when the period could not be parsed
};

struct Spec {
    std::string section, symbol, definition, comment;
    std::string key() const { return section + "." + symbol; }
};

struct Connection {
    std::string instrument;
    int channel = -1;
};

struct Pinmap {
    std::vector<std::string> instruments;
    std::vector<std::string> pins;
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    std::map<std::string, Connection> connections;
};

struct LevelRow {
    std::string pin;
    std::map<std::string, std::string> fields;   // Vil, Vih, ..., TerminationMode
};

Pinmap load_pinmap(const std::string& path);
std::vector<Spec> load_specs(const std::string& path);
std::map<std::string, double> spec_symbols(const std::vector<Spec>& specs);
// Copy of a .specs file with the definitions of {"DC.VDD": "3.0 V", ...} replaced.
void write_specs(const std::string& src, const std::string& dst, const std::map<std::string, std::string>& values);
std::vector<LevelRow> load_levels(const std::string& path);
std::vector<std::pair<std::string, Timeset>> load_timing(const std::string& path);

struct RunResult {
    double total = 0;
    std::vector<std::pair<double, std::string>> timeline;   // (t, states) at each change
    std::vector<std::pair<double, double>> waits;            // (t, duration) of busy waits
    double outer_iteration = -1;                             // < 0: no outer program loop
    int outer_loops = 0;
    int outer_set_loop = -1;
    bool extrapolated = false;
    std::vector<std::string> notes;
    long steps = 0;
};

class Pattern {
public:
    explicit Pattern(const std::string& path);

    std::string path, file, name;            // name = burst start label
    std::vector<std::string> pins, timesets, comments, label_names, states;
    std::vector<int> label_locs;
    std::vector<uint32_t> base_ins;
    std::map<size_t, int> edits;             // vector index -> new operand
    double mtime = 0;
    dm::Json meta;                           // generator sidecar {"params","roles","summary"}
    bool generated = false;
    std::string data;
    tdms::File tdms;

    std::vector<uint32_t> ins() const;
    bool modified() const;
    std::string label_at(size_t i) const;
    std::string section_of(size_t i) const;  // nearest label at or above vector i
    std::string instruction_text(size_t i) const;
    int loop_end(size_t i) const;            // end_loop closing the set_loop at i, -1 if none
    std::string timeset_of(size_t i) const;
    // Returns (applied value, clamped?). Throws std::invalid_argument for vectors without a count.
    std::pair<int, bool> set_count(size_t i, long long value);
    std::string patched_bytes() const;
    RunResult run(const std::map<std::string, double>& periods, double busy_delay, std::mt19937* rng = nullptr,
                  double record_limit = 20e-3, long max_steps = 400000) const;
};

// TDC timing of a pattern: pin roles, START / HIT timing, loops, run time, events and the
// timeline in the scope widget format. Missing values are NaN (JSON null).
dm::Json analyze(const Pattern& p, const std::map<std::string, double>& periods, double busy_delay);

class Project {
public:
    Project(const std::string& folder, std::vector<std::string> extra_dirs);

    std::string folder, pinmap_path, specs_path, levels_path, timing_path;
    std::vector<std::string> extra_dirs;
    Pinmap pinmap;
    std::vector<Spec> specs;
    std::vector<LevelRow> levels;
    std::vector<std::pair<std::string, Timeset>> timing;     // file order, first definition wins
    std::map<std::string, std::string> timing_file;          // timeset -> .digitiming defining it
    std::vector<std::shared_ptr<Pattern>> patterns;          // sorted by name (case-insensitive)

    void rescan();
    std::shared_ptr<Pattern> find(const std::string& name) const;
    std::map<std::string, double> periods() const;
    std::vector<std::string> timing_files() const;           // every .digitiming, project first
};

} // namespace tdc
