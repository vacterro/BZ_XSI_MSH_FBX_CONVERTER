# SAIHANDOFF-11 regression set (T-9). Every case below was independently
# reproduced against the audited snapshot and must FAIL before the fix and
# PASS after it. Compiles the whole engine from source (fresh build), runs
# the full unit suite, and requires the dedicated T-9 markers to pass.
# Pillow is the independent cross-check decoder for the image fixtures.
import io
import pathlib
import subprocess
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
CXX = r"C:\ProgramData\mingw64\mingw64\bin\g++.exe"
try:
    from PIL import Image
except ImportError:
    Image = None


def build_and_run_suite():
    sources = sorted(ROOT.glob("src/bzconverter/**/*.cpp"))
    sources.append(ROOT / "tests/test_main.cpp")
    exe = ROOT / "bzconv_tests.exe"
    cmd = [CXX, "-std=c++20", "-I", str(ROOT / "src"), "-o", str(exe)]
    cmd += [str(p) for p in sources] + ["-lz"]
    build = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if build.returncode != 0:
        raise AssertionError("build failed:\n" + build.stderr[-4000:])
    bindir = pathlib.Path(CXX).parent
    bridge = ROOT / "src/bzconverter/exporters/blender_scripts/bz_fbx_bridge.py"
    (ROOT / "bz_fbx_bridge.py").write_bytes(bridge.read_bytes())
    for dll in bindir.glob("lib*-6.dll"):
        (ROOT / dll.name).write_bytes(dll.read_bytes())
    for dll in bindir.glob("libwinpthread*.dll"):
        (ROOT / dll.name).write_bytes(dll.read_bytes())
    run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=900)
    # A failing suite is the RED half of a pair: surface it through test
    # assertions, never a setUpClass crash that would erase the executed-test
    # count the regression gate requires.
    return run.stdout + run.stderr


T9_MARKERS = (
    "t9_world_space_normal_reconstruction",
    "t9_missing_joint_attributes",
    "t9_invalid_jpeg_rejected",
    "t9_valid_indexed_png_accepted",
    "t9_cycle_not_serialized",
    "t9_instance_localization",
)


class P0Regression(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.output = build_and_run_suite()

    def test_01_world_space_normal_reconstruction(self):
        """P0: reconstructed world normals must match source world normals
        across the transform matrix (identity, rot, nonuniform scale,
        mirror, nesting)."""
        self.assertIn(f"[T9] t9_world_space_normal_reconstruction PASSED", self.output)

    def test_02_missing_joint_attributes(self):
        """P0: a skin reference must never be exported without matching
        JOINTS_n/WEIGHTS_n attributes."""
        self.assertIn(f"[T9] t9_missing_joint_attributes PASSED", self.output)

    def test_03_invalid_jpeg_rejected(self):
        """P0: structurally complete but undecodable JPEG must be
        GLTF_TEXTURE_INVALID (real decode, cross-checked with Pillow)."""
        if Image is not None:
            # witness that the fixture really is undecodable
            import tempfile

            bad = pathlib.Path(tempfile.mkdtemp(prefix="bzconv_t9_")) / "bad.jpg"
            bad.write_bytes(
                bytes([0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00])
                + bytes([16] * 64)
                + bytes([0xFF, 0xC0, 0x00, 0x0F, 0x00, 0, 4, 0, 4, 3, 1, 0x22, 0, 2, 0x11, 1, 3, 0x11, 1])
                + bytes([0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00])
                + bytes([0x11] * 8)
                + bytes([0xFF, 0xD9])
            )
            with self.assertRaises(Exception):
                Image.open(io.BytesIO(bad.read_bytes())).verify()
        self.assertIn(f"[T9] t9_invalid_jpeg_rejected PASSED", self.output)

    def test_04_valid_indexed_png_accepted(self):
        """P1: indexed 1/2/4-bit PNGs decode with Pillow and must be
        accepted by the converter, not rejected as corrupt."""
        if Image is not None:
            for bits in (1, 2, 4):
                img = Image.new("P", (4, 4))
                img.putpalette([0, 0, 0, 255, 0, 0, 0, 255, 0, 255, 255, 0])
                img.putpixel((0, 0), 3)
                buf = io.BytesIO()
                img.save(buf, format="PNG", bits=bits)
                Image.open(io.BytesIO(buf.getvalue())).verify()
        self.assertIn(f"[T9] t9_valid_indexed_png_accepted PASSED", self.output)

    def test_05_cycle_not_serialized(self):
        """P1: a detected hierarchy cycle must be removed from the exported
        node children, not published as-is."""
        self.assertIn(f"[T9] t9_cycle_not_serialized PASSED", self.output)

    def test_06_instance_localization(self):
        """P1: a model-space mesh referenced from two nodes at different
        translations must localize per instance, not to the first node."""
        self.assertIn(f"[T9] t9_instance_localization PASSED", self.output)

    def test_00_full_suite_green(self):
        """Whole existing suite must stay green alongside the fixes."""
        self.assertIn(" PASSED", self.output)
        self.assertNotIn("  FAILED:", self.output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
