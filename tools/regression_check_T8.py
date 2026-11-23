import pathlib
import subprocess
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE if (HERE / "src").is_dir() else HERE.parent
CXX = r"C:\ProgramData\mingw64\mingw64\bin\g++.exe"


class P0Regression(unittest.TestCase):
    def test_glb_influence_limit_matrix(self):
        sources = sorted(ROOT.glob("src/bzconverter/**/*.cpp"))
        sources.append(ROOT / "tests/test_main.cpp")
        self.assertTrue(sources)
        exe = ROOT / "bzconv_tests.exe"
        cmd = [CXX, "-std=c++20", "-I", str(ROOT / "src"), "-o", str(exe)]
        cmd += [str(p) for p in sources] + ["-lz"]
        build = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        self.assertEqual(build.returncode, 0, build.stderr[-4000:])
        bindir = pathlib.Path(CXX).parent
        bridge = ROOT / "src/bzconverter/exporters/blender_scripts/bz_fbx_bridge.py"
        (ROOT / "bz_fbx_bridge.py").write_bytes(bridge.read_bytes())
        for dll in bindir.glob("lib*-6.dll"):
            (ROOT / dll.name).write_bytes(dll.read_bytes())
        for dll in bindir.glob("libwinpthread*.dll"):
            (ROOT / dll.name).write_bytes(dll.read_bytes())
        run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=600)
        self.assertEqual(run.returncode, 0, run.stdout[-4000:] + run.stderr[-4000:])


if __name__ == "__main__":
    unittest.main(verbosity=2)
