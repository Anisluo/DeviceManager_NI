// workspace.hpp — new Digital Pattern Editor projects and export of a project as a task plugin.
//
// An exported plugin is a folder the host can load like any other: a copy of this DLL under the
// plugin's name, plugin.json, page.json and a copy of the project. No compiler is involved, the
// DLL is the runner and the project is its data.
#pragma once

#include <string>
#include <vector>

namespace tdc {

// Project with eight pins, one timeset and a pattern `main` holding one halt vector. `seed` is any
// compiled .digipat: it supplies the file format. Throws std::invalid_argument when `folder`
// already holds a project.
void new_project(const std::string& folder, const std::string& seed, const std::string& instrument);

struct ExportSpec {
    std::string id, name, version;           // plugin id (also the DLL name), display name
    std::string pattern;                     // pattern selected when the plugin opens
    std::string project;                     // project folder to copy
    std::string page;                        // page.json the exported page is derived from
    std::string library;                     // this DLL
    std::string root;                        // plugin folder the export lands in
};

// Writes <root>/<id>/ and returns that path. An earlier export of the same id is replaced; any
// other folder of that name is left alone and throws std::invalid_argument.
std::string export_plugin(const ExportSpec& spec);

} // namespace tdc
