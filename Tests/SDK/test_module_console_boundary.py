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
# MOD-310: every SparkGameFPS source logs and prints to the in-game console through
# Spark/ModuleLog.h (Source/Core/FPSLog.h), never through the engine's macros.
FPS_SOURCE = ROOT / "GameModules" / "SparkGameFPS" / "Source"
FPS_PRIVATE_LOG_HEADERS = (
    "Utils/LogMacros.h",
    "Utils/ConsoleProcessManager.h",
)
FPS_PRIVATE_LOG_TOKEN = re.compile(r"\b(LOG_TO_CONSOLE\w*|SPARK_LOG_\w+)\b")


def _fps_logging_violations_in_text(relative: str, text: str) -> list[str]:
    """Engine-private logging a SparkGameFPS source still uses, with comments and literals ignored."""
    findings: list[str] = []
    code, code_without_literals = module_content._lex_cpp(text)
    for match in PRIVATE_INCLUDE_PATTERN.finditer(code):
        header = match.group(1) or match.group(2)
        if header in FPS_PRIVATE_LOG_HEADERS:
            findings.append(f"{relative}: private log header {header}")
    for token in sorted(set(FPS_PRIVATE_LOG_TOKEN.findall(code_without_literals))):
        findings.append(f"{relative}: engine log macro {token}")
    return findings


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

    def test_fps_sources_log_through_the_public_sdk(self) -> None:
        sources = sorted(path for path in FPS_SOURCE.rglob("*") if path.is_file() and path.suffix in SOURCE_SUFFIXES)
        self.assertGreater(len(sources), 50)
        findings: list[str] = []
        for path in sources:
            relative = path.relative_to(ROOT).as_posix()
            findings.extend(_fps_logging_violations_in_text(relative, path.read_text(encoding="utf-8", errors="replace")))
        self.assertEqual([], findings)

    def test_fps_logging_mutations_fail_by_name(self) -> None:
        source = '#include "Utils/ConsoleProcessManager.h"\nvoid F() { SPARK_LOG_INFO(c, "x"); LOG_TO_CONSOLE(L"y", L"INFO"); }\n'
        self.assertEqual(
            [
                "Source/Probe.cpp: private log header Utils/ConsoleProcessManager.h",
                "Source/Probe.cpp: engine log macro LOG_TO_CONSOLE",
                "Source/Probe.cpp: engine log macro SPARK_LOG_INFO",
            ],
            _fps_logging_violations_in_text("Source/Probe.cpp", source),
        )
        self.assertEqual([], _fps_logging_violations_in_text("Source/Probe.cpp", '// SPARK_LOG_INFO\nconst char* s = "LOG_TO_CONSOLE";\n'))

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
