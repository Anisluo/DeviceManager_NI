"""Portable NI Digital Pattern Editor projects, using the bundled native project as schema."""

from pathlib import Path
import os
import re
import shutil
import tempfile
import xml.etree.ElementTree as ET

PF = "http://www.ni.com/PlatformFramework"
PD = "http://www.ni.com/PatternBasedDigital"
TYPES = {".pinmap": "PinMap", ".specs": "SpecificationsDefinition",
         ".digilevels": "PinLevelsDefinition", ".digitiming": "TimingDefinition",
         ".digipat": "DigitalPatternEditor.PatternDefinition",
         ".digicapture": "CaptureWaveformDefinition", ".tdms": "SourceWaveformDefinition"}
ET.register_namespace("", PF)
ET.register_namespace("digital", PD)


def project_file(path):
    path = Path(path).expanduser().resolve()
    if path.is_file() and path.suffix.lower() == ".digiproj":
        return path
    files = sorted(path.glob("*.digiproj")) if path.is_dir() else []
    if len(files) != 1:
        raise ValueError("请选择一个 .digiproj 文件（或只含一个工程文件的目录）")
    return files[0]


def write_project(folder, template):
    """Rebuild file references; retain NI version metadata and embedded project definitions."""
    folder = Path(folder).resolve()
    existing = list(folder.glob("*.digiproj"))
    manifest = existing[0] if len(existing) == 1 else folder / (folder.name + ".digiproj")
    tree = ET.parse(existing[0] if len(existing) == 1 else template)
    root = tree.getroot()
    root.attrib.pop("Checksum", None)  # The template checksum no longer describes this project.
    target = root.find(f".//{{{PF}}}NameScopingEnvoy")
    if target is None:
        raise ValueError("不是 Digital Pattern Editor 工程：缺少 DefaultTarget")
    active = {}
    for parent in root.iter():
        for node in list(parent):
            if node.tag.rsplit("}", 1)[-1] in ("FileReference", "SourceFileReference"):
                ext = Path(node.get("StoragePath", "").replace("\\", "/")).suffix.lower()
                if existing and ext not in TYPES:
                    continue
                if node.get(f"{{{PD}}}IsActive", "").lower() == "true":
                    active[Path(node.get("StoragePath", "").replace("\\", "/")).suffix.lower()] = node.get("StoragePath", "").replace("\\", "/")
                parent.remove(node)
    next_id = max((int(n.get("Id")) for n in root.iter() if n.get("Id", "").isdigit()), default=10) + 1
    seen = set()
    for file in sorted(folder.rglob("*")):
        ext = file.suffix.lower()
        rel = file.relative_to(folder).as_posix()
        if not file.is_file() or ext not in TYPES or any(p.startswith(".") or p == "_work" for p in file.relative_to(folder).parts):
            continue
        attrs = {"Id": str(next_id), "ModelDefinitionType": TYPES[ext],
                 "Name": re.escape(file.name), "StoragePath": rel.replace("/", "\\")}
        if ext in (".pinmap", ".digilevels", ".digitiming"):
            if rel == active.get(ext) or (ext not in active and ext not in seen):
                attrs[f"{{{PD}}}IsActive"] = "True"
        ET.SubElement(target, f"{{{PF}}}FileReference", attrs)
        seen.add(ext)
        next_id += 1
    ET.indent(tree, space="\t")
    tree.write(manifest, encoding="utf-8", xml_declaration=True)
    return str(manifest)


def copy_project(source, destination, template=None):
    """Stage a complete independent project; never overwrite an existing destination."""
    source = project_file(source)
    destination = Path(destination).expanduser().resolve()
    if destination.exists():
        raise ValueError("目标已存在，请选择一个新目录：" + str(destination))
    if source.parent == destination or source.parent in destination.parents:
        raise ValueError("目标目录不能位于源工程内部")
    tree = ET.parse(source)
    references = [n for n in tree.getroot().iter() if n.tag.rsplit("}", 1)[-1] in ("FileReference", "SourceFileReference")]
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".digital-project-", dir=destination.parent) as tmp:
        stage = Path(tmp) / "project"
        shutil.copytree(source.parent, stage, ignore=shutil.ignore_patterns("_work", ".shadow", ".git"))
        for i, ref in enumerate(references):
            raw = ref.get("StoragePath", "")
            if not raw:
                raise ValueError("工程引用缺少 StoragePath")
            file = (source.parent / raw.replace("\\", "/")).resolve()
            if not file.is_file():
                raise ValueError("工程引用文件不存在：" + str(file))
            try:
                rel = file.relative_to(source.parent)
            except ValueError:
                rel = Path("External") / str(i) / file.name
            dest = stage / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(file, dest)
            ref.set("StoragePath", rel.as_posix().replace("/", "\\"))
        tree.getroot().attrib.pop("Checksum", None)
        tree.write(stage / source.name, encoding="utf-8", xml_declaration=True)
        if template:
            write_project(stage, template)
        # Directory rename publishes only after every reference has been copied successfully.
        os.rename(stage, destination)
    return str(destination / source.name)
