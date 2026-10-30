// sheets.hpp — text-preserving edits of the editor's XML sheets (.digitiming, .digilevels, .pinmap).
//
// Only the touched element changes, so formatting, line endings, comments and everything the
// plugin does not model stay as Digital Pattern Editor wrote them. Every function edits the file
// text in place and throws std::invalid_argument when the element is not there.
#pragma once

#include <string>
#include <vector>

namespace tdc {

// ---- .digitiming ----
void timing_set_period(std::string& x, const std::string& timeset, const std::string& text);
// key: On, Data, Return, Off, Strobe
void timing_set_edge(std::string& x, const std::string& timeset, const std::string& pin, const std::string& key,
                     const std::string& text);
// format: NR, RL, RH, SBC. Edges carry over; `default_return` is used when the old format had no Return.
void timing_set_format(std::string& x, const std::string& timeset, const std::string& pin, const std::string& format,
                       const std::string& default_return);
// New entry for `pin`: a copy of the entry of `copy_from`, or NR at 0 when `copy_from` is empty.
void timing_add_pin(std::string& x, const std::string& timeset, const std::string& pin, const std::string& copy_from);
void timing_remove_pin(std::string& x, const std::string& timeset, const std::string& pin);
void timing_add_timeset(std::string& x, const std::string& name, const std::string& copy_of);
void timing_remove_timeset(std::string& x, const std::string& name);

// ---- .digilevels ----
void levels_set(std::string& x, const std::string& pin, const std::string& field, const std::string& text);
void levels_add(std::string& x, const std::string& pin, const std::string& copy_of);
void levels_remove(std::string& x, const std::string& pin);

// ---- .specs ----
// field: Definition, Comment
void specs_set(std::string& x, const std::string& section, const std::string& symbol, const std::string& field,
               const std::string& text);
void specs_add(std::string& x, const std::string& section, const std::string& symbol, const std::string& definition);
void specs_remove(std::string& x, const std::string& section, const std::string& symbol);

// ---- .pinmap ----
void pinmap_add_pin(std::string& x, const std::string& pin);
void pinmap_remove_pin(std::string& x, const std::string& pin);          // with its group members and connections
void pinmap_set_connection(std::string& x, const std::string& pin, const std::string& instrument, int channel);
void pinmap_set_groups(std::string& x, const std::string& pin, const std::vector<std::string>& groups);
void pinmap_rename_instrument(std::string& x, const std::string& from, const std::string& to);

} // namespace tdc
