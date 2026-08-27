from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath


SCRIPT = Path(__file__).resolve().parents[2] / "docs" / "codebase-metrics.py"
SPEC = importlib.util.spec_from_file_location("codebase_metrics", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class CodebaseMetricsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.tracked: list[PurePosixPath] = []

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write(self, relative: str, content: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8", newline="\n")
        self.tracked.append(PurePosixPath(relative))

    def test_collects_the_complete_tracked_first_party_native_corpus(self) -> None:
        self.write("SparkEngine/Source/Core.cpp", "TEST(CoreCase)\nint core;\n")
        self.write("Tests/TestMetal.mm", "TEST_F(MetalCase)\n")
        self.write("Templates/Example/Gameplay.inl", "inline void Play() {}\n")
        self.write("SparkBuild/src/main.cpp", "int main() {}\n")
        self.write("ThirdParty/Vendor/vendor.cpp", "int vendor;\n")
        self.write("build/generated.cpp", "int generated;\n")
        self.write("notes.txt", "not native source\n")
        metrics = MODULE.collect(self.root, tracked_paths=self.tracked)

        self.assertEqual(metrics["file_count"], 4)
        self.assertEqual(metrics["total_lines"], 5)
        self.assertEqual(metrics["engine_lines"], 2)
        self.assertEqual(metrics["game_lines"], 1)
        self.assertEqual(metrics["test_lines"], 1)
        self.assertEqual(metrics["tool_lines"], 1)
        self.assertEqual(metrics["test_definitions"], 2)
        self.assertEqual(metrics["test_files"], 2)

    def test_rejects_an_unclassified_native_source(self) -> None:
        self.write("NewFirstPartyRuntime/Source/NewRuntime.cpp", "int runtime;\n")
        with self.assertRaisesRegex(ValueError, "not classified"):
            MODULE.collect(self.root, tracked_paths=self.tracked)

    def test_current_repository_inventory_is_fully_classified(self) -> None:
        repo_root = SCRIPT.parents[1]
        sources = MODULE.tracked_native_sources(repo_root)
        categories = [MODULE.classify(path) for path in sources]
        self.assertTrue(sources)
        self.assertNotIn(None, categories)

    def test_json_schema_is_numeric_and_round_trips(self) -> None:
        metrics = MODULE.collect(SCRIPT.parents[1])
        self.assertEqual(json.loads(json.dumps(metrics, sort_keys=True)), metrics)
        self.assertTrue(all(isinstance(value, int) for value in metrics.values()))


if __name__ == "__main__":
    unittest.main()
