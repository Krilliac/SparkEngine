#!/usr/bin/env python3
"""Public-SDK console and logging boundary for prototype game modules.

The v7 SDK exposes ``IEngineContext::GetConsole`` and ``GetLogger``.  Module
sources must use ``Spark/IConsole.h`` and ``Spark/ModuleLog.h`` instead of
reaching into the engine's private console and logging singletons.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
import sys

sys.path.insert(0, str(ROOT / "tools" / "site-data"))
import module_content  # noqa: E402


MODULES = (
    "SparkGameARPG",
    "SparkGameOpenWorld",
    "SparkGamePlatformer",
    "SparkGameRacing",
    "SparkGameRPG",
    "SparkGameRTS",
)
FPS_BOUNDARY_FILES = (
    "GameModules/SparkGameFPS/Source/Core/Main.cpp",
    "GameModules/SparkGameFPS/Source/Core/HeadlessPersistence.cpp",
)
SOURCE_SUFFIXES = module_content.INCLUDE_SOURCE_SUFFIXES
PRIVATE_CONSOLE_HEADERS = (
    "Utils/SparkConsole.h",
    "Utils/LogMacros.h",
)
PRIVATE_INCLUDE_PATTERN = re.compile(
    r"^[ \t]*#[ \t]*include[ \t]*(?:\"([^\"\n]+)\"|<([^>\n]+)>)",
    re.MULTILINE,
)


def _violations_in_text(relative: str, text: str) -> list[str]:
    """Scan one source payload, with comments and literals ignored."""
    findings: list[str] = []
    code, code_without_literals = module_content._lex_cpp(text)
    for match in PRIVATE_INCLUDE_PATTERN.finditer(code):
        header = match.group(1) or match.group(2)
        if header in PRIVATE_CONSOLE_HEADERS:
            findings.append(f"{relative}: private console/log header {header}")
    if re.search(r"\bSimpleConsole\b", code_without_literals):
        findings.append(f"{relative}: SimpleConsole reference")
    return findings


def _violations(module_root: Path) -> list[str]:
    """Return source-boundary violations for a module."""
    source = module_root / "Source"
    findings: list[str] = []
    for path in sorted(source.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            findings.extend(
                _violations_in_text(
                    path.relative_to(module_root).as_posix(),
                    path.read_text(encoding="utf-8", errors="replace"),
                )
            )
    return findings


class ModuleConsoleBoundaryTests(unittest.TestCase):
    def test_module_source_trees_are_present_and_nonempty(self) -> None:
        for name in MODULES:
            with self.subTest(module=name):
                source = ROOT / "GameModules" / name / "Source"
                self.assertTrue(source.is_dir())
                self.assertTrue(any(path.is_file() for path in source.rglob("*")))

    def test_prototype_modules_use_public_console_and_logging_surfaces(self) -> None:
        for name in MODULES:
            with self.subTest(module=name):
                self.assertEqual([], _violations(ROOT / "GameModules" / name))

    def test_fps_core_boundary_files_use_public_console_and_logging_surfaces(self) -> None:
        for relative in FPS_BOUNDARY_FILES:
            with self.subTest(source=relative):
                path = ROOT / relative
                self.assertTrue(path.is_file())
                self.assertEqual(
                    [],
                    _violations_in_text(
                        relative,
                        path.read_text(encoding="utf-8", errors="replace"),
                    ),
                )

    def test_private_include_mutation_fails_by_header_name(self) -> None:
        source = ROOT / "GameModules" / "SparkGameRTS" / "Source" / "Core" / "Main.cpp"
        mutated = '#include "Utils/SparkConsole.h"\n' + source.read_text(encoding="utf-8")
        self.assertIn(
            "Source/Core/Main.cpp: private console/log header Utils/SparkConsole.h",
            _violations_in_text("Source/Core/Main.cpp", mutated),
        )

    def test_simple_console_mutation_fails_by_symbol(self) -> None:
        source = ROOT / "GameModules" / "SparkGameRTS" / "Source" / "Core" / "Main.cpp"
        mutated = "void UsesLegacyConsole() { SimpleConsole::GetInstance(); }\n" + source.read_text(encoding="utf-8")
        self.assertIn(
            "Source/Core/Main.cpp: SimpleConsole reference",
            _violations_in_text("Source/Core/Main.cpp", mutated),
        )

    def test_comments_and_string_literals_do_not_trigger(self) -> None:
        source = (
            '// #include "Utils/SparkConsole.h"\n'
            'const char* text = "SimpleConsole Utils/LogMacros.h";\n'
            "/* SimpleConsole */\n"
        )
        self.assertEqual([], _violations_in_text("Source/Boundary.cpp", source))


if __name__ == "__main__":
    unittest.main()
