// tdcgen.hpp — four-pin TDC stimulus generator, written straight to a compiled .digipat.
//
//   CLK   continuous clock: drive format return-to-low (<ReturnToLow>, as the editor writes it),
//         state 1 in every vector, so it pulses once per cycle (also inside repeats) at the
//         timeset period = 1 / clock frequency
//   TRIG  input: wait for a low->high (or high->low) edge with the same compare + jump_if wait
//         loop the DemoPrj-TDC patterns use (WaitForBusy)
//   OUT1  square wave, f1 for n1 cycles, starting d1 after the trigger
//   OUT2  square wave, f2 for n2 cycles, starting d2 after the trigger
// then idle for a while, and repeat the whole measurement `loops` times. Edges are quantised to
// the clock period; the achieved frequencies and delays are reported.
#pragma once

#include <dm/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace tdc {

struct GenError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Wave {
    long half = 1, delay = 0, cycles = 1;   // in clock cycles
    double freq = 0, start = 0, end = 0;    // Hz, s, s
};

struct GenPlan {
    dm::Json params;                        // the parameters actually used (defaults filled in)
    std::string name, timeset, edge;
    std::string clk, trig, out1, out2;      // pins ("" = CLK unused)
    double period = 0;
    Wave w1, w2;
    long post = 1;
    int loops = 1;
    std::vector<std::string> warnings;
};

struct GenVector {
    std::string label;
    std::string states;                     // one char per pattern pin
    uint32_t ins = 0;
    std::string comment;
};

dm::Json gen_defaults();
GenPlan gen_plan(const dm::Json& params, const std::vector<std::string>& pins_available);
std::vector<std::string> gen_pins(const GenPlan& pl);           // CLK, TRIG, OUT1, OUT2 (CLK may be absent)
std::vector<GenVector> gen_vectors(const GenPlan& pl);
// .digipat bytes, using a compiled pattern as the layout template.
std::string gen_build(const GenPlan& pl, const std::string& template_bytes);
// Writes <folder>/<timeset>.digitiming; returns its path.
std::string gen_timing_sheet(const GenPlan& pl, const std::vector<std::string>& all_pins, const std::string& folder);
dm::Json gen_summary(const GenPlan& pl);
std::string gen_note(const GenPlan& pl);

} // namespace tdc
