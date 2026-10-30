// Plugin template: copy sdk/template to devices/<id>/, then rename my_plugin
// in plugin.json, CMakeLists.txt and this file.
#include <dm/plugin.hpp>

#include <cmath>
#include <random>

static const char* kManifest = R"({
  "id": "my_plugin",
  "name": "My Plugin",
  "version": "0.1.0",
  "type": "cpp",
  "abi": 1,
  "capabilities": ["simulated"]
})";

namespace {

class MyPlugin : public dm::Plugin {
public:
    explicit MyPlugin(const dm::Json& cfg) : simulate_(cfg.boolean("simulate", true)) {
        // Real instruments: open the driver session for cfg.str("resource") here and
        // throw on failure (plugin_create then returns NULL and the host shows the error).

        method("set_level", "Set output level", {"level"}, [this](const dm::Json& a) {
            double v = a.number("level", NAN);
            dm::require(v == v, "level required");
            level_ = v;
            return dm::Json{{"ok", true}};
        });

        method("get_level", "Read back output level", {}, [this](const dm::Json&) {
            return dm::Json{{"level", level_}};
        });

        method("read", "Measure", {}, [this](const dm::Json&) {
            std::normal_distribution<double> noise(0.0, 1e-4);
            return dm::Json{{"value", level_ + noise(rng_)}, {"simulated", simulate_}};
        });
    }

private:
    bool simulate_;
    double level_ = 0.0;
    std::mt19937 rng_{42};
};

} // namespace

DM_EXPORT_PLUGIN(MyPlugin, kManifest)
