#include "workspace.hpp"
#include "project.hpp"
#include "util.hpp"

#include <dm/json.hpp>

#include <stdexcept>

namespace tdc {

namespace {

const char* kTimingXml = R"(<?xml version="1.0" encoding="utf-8"?>
<TimingFile xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" schemaVersion="1.0" xmlns="http://www.ni.com/Semiconductor/Timing">
  <TimingSheet>
    <TimeSets>
      <TimeSet name="default">
        <Period>Time.period</Period>
        <PinEdges>
          <PinEdge pin="AllPins">
            <DriveNonReturn>
              <On>0</On>
              <Data>0</Data>
              <Off>0</Off>
            </DriveNonReturn>
            <CompareStrobe>
              <Strobe>Time.period/2</Strobe>
            </CompareStrobe>
            <DataSource>Pattern</DataSource>
          </PinEdge>
        </PinEdges>
      </TimeSet>
    </TimeSets>
  </TimingSheet>
</TimingFile>
)";

const char* kLevelsXml = R"(<?xml version="1.0" encoding="utf-8"?>
<PinLevelsFile xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" schemaVersion="1.0" xmlns="http://www.ni.com/Semiconductor/PinLevels">
  <PinLevelsSheet>
    <DigitalPinLevelSets>
      <DigitalPinLevelSet pin="AllPins">
        <Vil>0 V</Vil>
        <Vih>DC.VDD</Vih>
        <Vol>0.4*DC.VDD</Vol>
        <Voh>0.6*DC.VDD</Voh>
        <Iol>15 mA</Iol>
        <Ioh>-15 mA</Ioh>
        <Vterm>0 V</Vterm>
        <TerminationMode>HighZ</TerminationMode>
      </DigitalPinLevelSet>
    </DigitalPinLevelSets>
  </PinLevelsSheet>
</PinLevelsFile>
)";

const char* kSpecsXml = R"(<?xml version="1.0" encoding="utf-8"?>
<Specifications xmlns:f="http://www.ni.com/schemas/Semiconductor/Formula.xsd" schemaVersion="1.0" xmlns="http://www.ni.com/schemas/Semiconductor/Specifications.xsd">
  <Section name="DC">
    <f:Formula symbol="VDD">
      <f:Definition>3.3 V</f:Definition>
      <f:Comment>Power supply voltage</f:Comment>
    </f:Formula>
  </Section>
  <Section name="Time">
    <f:Formula symbol="period">
      <f:Definition>100 ns</f:Definition>
      <f:Comment>Vector period</f:Comment>
    </f:Formula>
  </Section>
</Specifications>
)";

std::string pinmap_xml(const std::vector<std::string>& pins, const std::string& instrument)
{
    const std::string inst = xml_escape(instrument);
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<PinMap xmlns=\"http://www.ni.com/TestStand/SemiconductorModule/PinMap.xsd\" "
                    "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" schemaVersion=\"1.2\">\n"
                    "\t<Instruments>\n\t\t<NIDigitalPatternInstrument name=\"" + inst + "\" numberOfChannels=\"32\" />\n\t</Instruments>\n"
                    "\t<Pins>\n";
    for (auto& pin : pins) x += "\t\t<DUTPin name=\"" + pin + "\" />\n";
    x += "\t</Pins>\n\t<PinGroups>\n\t\t<PinGroup name=\"AllPins\">\n";
    for (auto& pin : pins) x += "\t\t\t<PinReference pin=\"" + pin + "\" />\n";
    x += "\t\t</PinGroup>\n\t</PinGroups>\n\t<Sites>\n\t\t<Site siteNumber=\"0\" />\n\t</Sites>\n\t<Connections>\n";
    for (size_t k = 0; k < pins.size(); ++k)
        x += "\t\t<Connection pin=\"" + pins[k] + "\" siteNumber=\"0\" instrument=\"" + inst + "\" channel=\"" + std::to_string(k) + "\" />\n";
    return x + "\t</Connections>\n</PinMap>\n";
}

} // namespace

void new_project(const std::string& folder, const std::string& seed, const std::string& instrument)
{
    if (!find_files(folder, ".pinmap").empty()) throw std::invalid_argument("工程已存在：" + folder);
    if (!file_exists(seed)) throw std::invalid_argument("找不到图形模板 " + seed);
    const std::vector<std::string> pins{"I0", "I1", "I2", "I3", "I4", "I5", "I6", "I7"};
    auto put = [&folder](const std::string& rel, const std::string& text) {
        const std::string path = path_join(folder, rel);
        make_dirs(parent_dir(path));
        write_file(path, text);
    };
    put(base_name(folder) + ".pinmap", pinmap_xml(pins, instrument.empty() ? "PXI1Slot15" : instrument));
    put("Specifications/Specifications.specs", kSpecsXml);
    put("Levels/Levels.digilevels", kLevelsXml);
    put("Timing/Timing.digitiming", kTimingXml);
    const std::string main_path = path_join(folder, "Patterns/main.digipat");
    put("Patterns/main.digipat", read_file(seed));
    Pattern p(main_path);
    p.clear("main", pins, "default");
    write_file(main_path, p.patched_bytes());
}

std::string export_plugin(const ExportSpec& s)
{
    if (!valid_name(s.id)) throw std::invalid_argument("插件 id '" + s.id + "' 无效：只能用字母、数字和下划线，且不能以数字开头");
    if (!file_exists(s.library)) throw std::invalid_argument("找不到运行器 DLL " + s.library);
    const std::string dest = path_join(s.root, s.id);
    const std::string manifest_path = path_join(dest, "plugin.json");
    if (file_exists(dest)) {
        bool ours = false;
        try {
            ours = file_exists(manifest_path) && dm::Json::parse(read_file(manifest_path)).boolean("exported", false);
        } catch (const std::exception&) {
        }
        if (!ours) throw std::invalid_argument("文件夹 " + dest + " 已存在，且不是导出的插件，请换一个 id");
    }

    dm::Json page = dm::Json::parse(read_file(s.page));
    dm::Json tabs = dm::Json::array();
    for (auto& tab : page["tabs"].items())
        if (tab.str("title") != "信号生成") tabs.push_back(tab);
    page["tabs"] = tabs;
    page["title"] = s.name;
    page["subtitle"] = "导出的测试任务 · PXIe-6571";

    dm::Json models = dm::Json::array();
    models.push_back("PXIe-6571");
    dm::Json capabilities = dm::Json::array();
    capabilities.push_back("digital.pattern");
    dm::Json digital = dm::Json::object();
    digital["models"] = models;
    digital["doc"] = "运行图形用的数字图形仪器";
    dm::Json needs = dm::Json::object();
    needs["digital"] = digital;
    dm::Json options = dm::Json::object();
    options["project"] = "project";
    options["pattern"] = s.pattern;
    dm::Json defaults = dm::Json::object();
    defaults["options"] = options;
    dm::Json manifest{{"id", s.id}, {"name", s.name}, {"version", s.version}, {"type", "cpp"}, {"entry", s.id},
                      {"abi", 1}, {"category", "task"}, {"exported", true},
                      {"description", "由 PXIe-6571 图形工程导出的测试任务，工程在 project/ 里。"},
                      {"requires", needs}, {"capabilities", capabilities}, {"page", "page.json"},
                      {"default_config", defaults}};

    // Everything is ready: replace the old export only now, so a failure leaves it intact.
    remove_tree(path_join(dest, "project"));
    make_dirs(dest);
    copy_tree(s.project, path_join(dest, "project"));
    write_file(path_join(dest, s.id + library_suffix()), read_file(s.library));
    write_file(path_join(dest, "page.json"), page.dump());
    write_file(manifest_path, manifest.dump());
    write_file(path_join(dest, "README.md"),
               "# " + s.name + "\n\n由 PXIe-6571 图形工程导出的测试任务，`" + s.id + library_suffix() + "` 是通用运行器的副本，工程在 `project/` 里。\n"
               "把整个文件夹放到主程序的 `devices/` 下，点“重新扫描插件”即可加载。\n");
    return dest;
}

} // namespace tdc
