# T-14 regression set: source-fidelity oracle must remain green while the
# subject (source_fidelity_check.py) changes. The same verifier is measured
# against the pre-fix subject snapshot and the patched subject.
import pathlib
import subprocess
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent


class T14Regression(unittest.TestCase):
    def test_00_source_fidelity_oracle_all_green(self):
        run = subprocess.run(
            [sys.executable, str(HERE / "test_source_fidelity_oracle.py")],
            cwd=ROOT,
            capture_output=True,
            text=True,
            timeout=300,
        )
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("FIDELITY ORACLE RESULT: ALL GREEN", run.stdout)
        for case in (
            "material binding swap detected",
            "material binding removal detected",
            "texture channel loss detected",
            "middle animation key mutation detected",
        ):
            self.assertIn(f"[fidelity] {case}: PASS", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
