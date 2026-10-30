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
//
// Timing follows the editor's timing sheet: each timeset gives every pin (or pin group) a drive
// format and edges within the cycle. NR changes level at Data; RL drives a 1 as a pulse from Data
// to Return (so a repeated 1 is one pulse per cycle); RH is the mirror for 0; SBC drives the
// complement around the Data..Return window.
#pragma once

#include "tdms.hpp"

#include <dm/json.hpp>

#include <cstdint>
#include <limits>
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
unsigned state_code(char c);               // '0' -> 0, 'L' -> 3, ...; X for anything else

// One <PinEdge> of a timing sheet, as written.
struct PinEdge {
    std::string pin;                       // pin or pin group
    std::string format;                    // NR, RL, RH, SBC; "" when the entry has no drive edges
    std::map<std::string, std::string> edges;   // On, Data, Return, Off, Strobe -> text
    std::string comment;
};

struct Timeset {
    std::string period_text;
    double period = 0;                     // 0 when the period could not be parsed
    std::vector<PinEdge> edges;
};

// Edge times of one pin in one timeset, evaluated against the specifications [s from cycle start].
struct EdgeTimes {
    std::string format = "NR";
    double on = 0, data = 0, ret = std::numeric_limits<double>::quiet_NaN(), off = 0, strobe = 0;
    bool defined = false;                  // the timeset has an entry for this pin or one of its groups
};

struct TimesetModel {
    double period = 0;                     // 0 when the period could not be evaluated
    std::string sheet;                     // .digitiming defining it
    std::map<std::string, EdgeTimes> pins;
    std::vector<std::pair<std::string, std::string>> problems;   // (pin, message)
};
using TimingModel = std::map<std::string, TimesetModel>;

std::string format_name(const std::string& format);   // "RL" -> "RL 归零到低"

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
    std::map<std::string, int> channels;   // instrument -> numberOfChannels
    std::vector<std::string> pins;
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    std::map<std::string, Connection> connections;
};

// Levels of one pin in volts, formulas evaluated; what the simulated waveform draws.
struct PinVolts {
    double vil = 0, vih = 1, vol = 0, voh = 1, vterm = 0;
    bool terminated = false;               // TerminationMode Vterm: an undriven pin sits at vterm
    bool defined = false;                  // false: no levels for this pin, drawn from 0 to 1
    int channel = -1;                      // from the pin map, -1 when not connected
};
using PinVoltMap = std::map<std::string, PinVolts>;

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
    struct Seg { double t; size_t v; long cycles; double period; };
    double total = 0;
    std::vector<Seg> segs;                                   // executed vectors of the recorded part
    double record_end = 0;                                   // end of the recorded part
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
private:
    struct Snapshot {
        std::string name;
        std::vector<std::string> pins, timesets, comments, label_names, states;
        std::vector<int> label_locs, export_locs;
        std::vector<uint32_t> base_ins;
        std::map<size_t, int> edits;
        std::vector<double> group_index;
        bool structural = false;
    };
    std::vector<Snapshot> history_;
    void load();                             // decode `data`
    void begin_edit();                       // folds the count edits into the vector table
    size_t pin_index(const std::string& pin) const;
    size_t label_index(const std::string& label) const;
    std::string rebuilt_bytes() const;
public:
    std::vector<std::string> pins, timesets, comments, label_names, states;
    std::vector<int> label_locs, export_locs;
    std::vector<uint32_t> base_ins;
    std::map<size_t, int> edits;             // vector index -> new operand
    double mtime = 0;
    dm::Json meta;                           // generator sidecar {"params","roles","summary"}
    bool generated = false;
    std::string data;
    tdms::File tdms;
    bool structural = false;                 // the vector table was edited; bytes must be rebuilt
    unsigned revision = 0;                   // bumped by every edit

    // Vector table editing. Works on the fields above until commit(); invalid input throws
    // std::invalid_argument and changes nothing. Labels are referenced by index, so inserting and
    // deleting vectors leaves end_loop / jump_if targets intact.
    void set_state(size_t v, const std::string& pin, const std::string& state);
    // Same text as instruction_text(): "", repeat(n), set_loop(n), end_loop(label), match,
    // jump_if(!matched, label), halt, plus an optional "[flags 0x20]". A jump_if condition with no
    // known name is written as its flags, jump_if(0x28!, label) means the same as !matched.
    void set_instruction(size_t v, const std::string& text);
    void set_timeset(size_t v, const std::string& name);
    void set_label(size_t v, const std::string& name);       // empty: remove an unreferenced label
    void set_comment(size_t v, const std::string& text);
    void insert_vector(size_t after);                        // copy of the vector's pin states, no instruction
    void remove_vector(size_t v);
    void add_pin(const std::string& pin);                    // new column of X
    bool rename_pin(const std::string& from, const std::string& to);       // false: not in this pattern
    bool rename_timeset(const std::string& from, const std::string& to);
    void remove_pin(const std::string& pin);
    // Replace the content with one halt vector of X on `pins`, starting at `label`.
    void clear(const std::string& label, const std::vector<std::string>& pins, const std::string& timeset);
    void reset();                                            // discard every edit
    void commit(const std::string& bytes);                   // `bytes` are the file now
    void saved(const std::string& bytes);                    // same, keeps what undo() can go back to
    void checkpoint();                                       // remember the state for undo()
    bool undo();                                             // back to the last checkpoint, false if none

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
    std::vector<std::string> used_timesets() const;          // referenced by instructions
    bool label_referenced(size_t label) const;
    RunResult run(const TimingModel& tm, double busy_delay, std::mt19937* rng = nullptr,
                  double record_limit = 20e-3, long max_steps = 400000) const;
};

// TDC timing of a pattern: pin roles, START / HIT timing, loops, run time, events and the
// timeline in the scope widget format. Edges follow the drive formats of the timing model.
// Missing values are NaN (JSON null).
dm::Json analyze(const Pattern& p, const TimingModel& tm, double busy_delay, const PinVoltMap& volts = {});

class Project;
// What the editor would flag before running: pins outside the pin map or without a channel,
// pins without levels, timesets that do not define a pattern pin, edges outside the cycle.
std::vector<std::string> check(const Pattern& p, const Project& pr, const TimingModel& tm);

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

    void reload_sheets();                                    // pin map, levels and timing changed on disk
    void rescan();
    std::shared_ptr<Pattern> find(const std::string& name) const;
    std::vector<std::string> expand(const std::string& pin_or_group) const;
    // Every timeset with periods and edges evaluated against `symbols` (spec_symbols()).
    TimingModel model(const std::map<std::string, double>& symbols) const;
    TimingModel model() const;                               // against the project's specifications
    // Levels and channels of every pin; a pin's own entry wins over its group's.
    PinVoltMap pin_volts(const std::map<std::string, double>& symbols) const;
    std::map<std::string, double> periods() const;
    std::vector<std::string> timing_files() const;           // every .digitiming, project first
};

} // namespace tdc
