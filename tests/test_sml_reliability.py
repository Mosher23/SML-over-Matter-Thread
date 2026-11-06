"""Compile and run the real SML decoder and measurement tracker with sanitizers."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SmlReliabilityTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_decoder_mapping_and_virtual_clock(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / "sml-test"
            built = subprocess.run([
                shutil.which("c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                str(ROOT / "firmware/tests/test_sml_reliability.cpp"), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            subprocess.run([str(binary)], check=True)
