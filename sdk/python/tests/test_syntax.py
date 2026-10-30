"""Every Python file shipped with the host must at least compile: a syntax error in
dm/_host.py stops Python from starting at all (the host then runs without Python)."""

import glob
import os
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


class Syntax(unittest.TestCase):
    def test_all_python_files_compile(self):
        files = (glob.glob(os.path.join(ROOT, "sdk", "python", "**", "*.py"), recursive=True)
                 + glob.glob(os.path.join(ROOT, "devices", "**", "*.py"), recursive=True)
                 + glob.glob(os.path.join(ROOT, "sdk", "template_python", "*.py"))
                 + glob.glob(os.path.join(ROOT, "DeviceManager", "sequences", "*.py")))
        self.assertGreater(len(files), 10)
        for path in files:
            with self.subTest(path=os.path.relpath(path, ROOT)):
                with open(path, encoding="utf-8") as fh:
                    compile(fh.read(), path, "exec")


if __name__ == "__main__":
    unittest.main()
