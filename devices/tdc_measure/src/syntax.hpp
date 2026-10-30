// syntax.hpp — vector rules of the NI-Digital Pattern User Manual (Text Pattern File Syntax, Opcodes).
//
// Names follow "Identifiers and Reserved Keywords"; check_syntax() looks at a whole pattern the way
// the Digital Pattern Compiler would, plus the comparison rules the compiler leaves to the author.
#pragma once

#include "project.hpp"

#include <string>
#include <vector>

namespace tdc {

// Empty when `name` is a valid identifier, else why it is not.
std::string identifier_problem(const std::string& name);

// True for a name of the language that the compiled format here has no known encoding for.
bool is_unencoded_opcode(const std::string& name);

struct Issue {
    int vector = -1;                 // -1: the pattern as a whole
    bool error = false;              // false: allowed, but likely not what was meant
    std::string text;
};

std::vector<Issue> check_syntax(const Pattern& p);

} // namespace tdc
