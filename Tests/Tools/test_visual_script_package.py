from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest import mock


SCRIPT = Path(__file__).parents[1] / "run_visual_script_package.py"
SPEC = importlib.util.spec_from_file_location("run_visual_script_package", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class VisualScriptPackageContractTests(unittest.TestCase):
    def test_autoplay_timeline_is_fixed_and_covers_all_segments(self) -> None:
        self.assertEqual(
            MODULE.build_autoplay_script(),
            "0 vs_autoplay -10 8\n30 vs_autoplay -5 12\n60 vs_autoplay 0 8\n"
            "90 vs_autoplay 5 12\n120 vs_autoplay 10 8\n",
        )

    def test_validate_package_requires_runtime_module_sidecar_manifest_and_graph_output(self) -> None:
        root = Path("package")
        engine = root / "SparkEngine"
        module = root / "SparkGameVisualScript"
        with mock.patch.object(Path, "is_file", return_value=True), mock.patch.object(
            Path, "is_dir", return_value=True
        ), mock.patch.object(Path, "glob", return_value=[root / "Assets/Scripts/Generated/GameManager.as"]):
            self.assertEqual(MODULE.validate_package(root, engine, module), [])

    def test_validate_output_rejects_nonzero_and_duplicate_or_missing_wins(self) -> None:
        lifecycle = "SPARK_HEADLESS_LIFECYCLE initialized=1 updated=480 fixed=480 rendered=0 unloaded=1 faults=0\n"
        good = "log\n*** YOU WIN! ***\n" + lifecycle
        self.assertIsNone(MODULE.validate_output(good, 0))
        self.assertIsNotNone(MODULE.validate_output(good.replace(lifecycle, ""), 0))
        self.assertIsNotNone(MODULE.validate_output(good.replace("faults=0", "faults=1"), 0))
        self.assertIsNotNone(MODULE.validate_output(good + lifecycle, 0))
        self.assertIsNotNone(MODULE.validate_output(good + "ERROR: AddressSanitizer: heap-use-after-free", 0))
        self.assertEqual(MODULE.validate_output("log\n", 0), "expected exactly one '*** YOU WIN! ***', found 0")
        self.assertEqual(
            MODULE.validate_output("*** YOU WIN! ****** YOU WIN! ***", 0),
            "expected exactly one '*** YOU WIN! ***', found 2",
        )
        self.assertEqual(MODULE.validate_output("fault", 3), "packaged engine exited with status 3")


if __name__ == "__main__":
    unittest.main()
