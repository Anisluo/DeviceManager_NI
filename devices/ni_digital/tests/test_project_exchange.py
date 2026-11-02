"""Native project packaging: references, binary preservation, and failure atomicity."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from project_exchange import PF, copy_project, write_project

DEMO = Path(__file__).resolve().parents[2] / "tdc_measure/project/DemoPrj-TDC/myDemo.digiproj"


class ProjectExchange(unittest.TestCase):
    def test_native_project_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            imported = Path(copy_project(DEMO, Path(tmp) / "导入"))
            self.assertEqual(ET.parse(imported).getroot().tag, f"{{{PF}}}SourceFile")
            pattern = imported.parent / "Patterns/TDC_M20_2.digipat"
            self.assertEqual(pattern.read_bytes(), (DEMO.parent / "Patterns/TDC_M20_2.digipat").read_bytes())
            # A deleted pattern must disappear from the exported project tree.
            (imported.parent / "Patterns/TDC_002.digipat").unlink()
            generated = write_project(imported.parent, DEMO)
            exported = Path(copy_project(generated, Path(tmp) / "export"))
            refs = [n.get("StoragePath") for n in ET.parse(exported).getroot().iter() if n.get("StoragePath")]
            self.assertNotIn("Patterns\\TDC_002.digipat", refs)
            self.assertIn("SystemView_2.digisysview", refs)
            for ref in refs:
                self.assertTrue((exported.parent / ref.replace("\\", "/")).is_file(), ref)

    def test_external_reference_and_missing_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "src"
            src.mkdir()
            (root / "external.specs").write_text("external", encoding="utf-8")
            manifest = src / "test.digiproj"
            manifest.write_text(f'<SourceFile xmlns="{PF}"><Project><FileReference StoragePath="..\\external.specs" /></Project></SourceFile>')
            exported = Path(copy_project(manifest, root / "dest"))
            ref = next(n.get("StoragePath") for n in ET.parse(exported).getroot().iter() if n.get("StoragePath"))
            self.assertEqual((exported.parent / ref.replace("\\", "/")).read_text(), "external")
            with self.assertRaises(ValueError):
                copy_project(manifest, root / "dest")
            (root / "external.specs").unlink()
            with self.assertRaises(ValueError):
                copy_project(manifest, root / "missing")
            self.assertFalse((root / "missing").exists())
            with self.assertRaises(ValueError):
                copy_project(manifest, src / "nested")


if __name__ == "__main__":
    unittest.main()
