#!/usr/bin/env python3
"""MOD-300: tools/check-module-positioning.py keeps SparkGame labelled as a showcase.

Fixture repositories plant each mislabel the checker must reject -- the catalog row
that called SparkGame an FPS arena showcase, a prose claim that it is a finished
game, and a module README without its disclaimers -- and confirm what it must
leave alone: SparkGameFPS described as a shooter, paths and file names, and a
negated "not a finished game". The last case runs against the real repository.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "check-module-positioning.py"

GOOD_MODULE_README = (
    "# SparkGame\n\n"
    "SparkGame is the base showcase module; it is not a game.\n\n"
    "**Release classification:** experimental showcase, outside the stable-v1 release profile.\n"
)
GOOD_CATALOG = (
    "| Module | Description | Load Order |\n"
    "|--------|-------------|------------|\n"
    "| **SparkGame** | Engine-systems showcase and module template (not a game) | 999 |\n"
    "| **SparkGameFPS** | FPS arena shooter | 1000 |\n"
)


def _load_checker():
    spec = importlib.util.spec_from_file_location("check_module_positioning", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


CHECKER = _load_checker()


class PositioningFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="module-positioning-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.write("GameModules/SparkGame/README.md", GOOD_MODULE_README)
        self.write("GameModules/README.md", GOOD_CATALOG)
        self.write("README.md", "# SparkEngine\n")
        self.write("wiki/getting-started/Game-Modules.md", "# Game Modules\n")

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_checker(self) -> tuple[int, str]:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = CHECKER.main(["--root", str(self.root)])
        return status, output.getvalue()

    def test_clean_fixture_passes(self) -> None:
        status, output = self.run_checker()
        self.assertEqual(0, status, output)

    def test_fps_arena_catalog_row_fails(self) -> None:
        self.write(
            "GameModules/README.md",
            GOOD_CATALOG.replace(
                "Engine-systems showcase and module template (not a game) | 999",
                "FPS arena showcase (player, weapons, enemies, projectiles) | 1000",
            ),
        )
        status, output = self.run_checker()
        self.assertEqual(1, status)
        self.assertIn("GameModules/README.md:3: MOD-300 forbidden label 'FPS' for SparkGame", output)

    def test_finished_game_claim_fails(self) -> None:
        self.write("wiki/getting-started/Game-Modules.md", "# Game Modules\n\nSparkGame is a finished game.\n")
        status, output = self.run_checker()
        self.assertEqual(1, status)
        self.assertIn("wiki/getting-started/Game-Modules.md:3: MOD-300 forbidden label 'finished game'", output)

    def test_missing_disclaimer_fails(self) -> None:
        self.write(
            "GameModules/SparkGame/README.md", GOOD_MODULE_README.replace("; it is not a game", "")
        )
        status, output = self.run_checker()
        self.assertEqual(1, status)
        self.assertIn("disclaimer missing: 'it is not a game'", output)

    def test_missing_release_classification_fails(self) -> None:
        self.write(
            "GameModules/SparkGame/README.md",
            GOOD_MODULE_README.replace("**Release classification:** experimental showcase", "Status: showcase"),
        )
        status, output = self.run_checker()
        self.assertEqual(1, status)
        self.assertIn("disclaimer missing: '**Release classification:** experimental showcase'", output)

    def test_other_modules_paths_and_negations_pass(self) -> None:
        self.write(
            "wiki/getting-started/Game-Modules.md",
            "# Game Modules\n\n"
            "SparkGameFPS is a shooter and a playable game candidate.\n"
            "The FPS source moved out of `GameModules/SparkGame/` into its own module.\n"
            "Launch `SparkGame.dll` headless; SparkGame and the genre modules (FPS, RPG) build together.\n"
            "SparkGame is not a finished game.\n",
        )
        status, output = self.run_checker()
        self.assertEqual(0, status, output)


class RepositoryTests(unittest.TestCase):
    def test_repository_labels_sparkgame_as_a_showcase(self) -> None:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = CHECKER.main(["--root", str(REPO_ROOT)])
        self.assertEqual(0, status, output.getvalue())


if __name__ == "__main__":
    unittest.main()
