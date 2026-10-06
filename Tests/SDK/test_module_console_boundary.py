#!/usr/bin/env python3
"""Public-SDK console, logging and state-validation boundary for prototype game modules.

The v7 SDK exposes ``IEngineContext::GetConsole`` and ``GetLogger``.  Module
sources must use ``Spark/IConsole.h`` and ``Spark/ModuleLog.h`` instead of
reaching into the engine's private console and logging singletons.

The v9 SDK exposes ``IEngineContext::GetStateValidation``.  Module sources must
add their invalid-state rules through ``Spark/IStateValidation.h`` instead of the
engine-private ``Utils/InvalidStateDetector.h`` singleton.
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
    "SparkGame",
    "SparkGameARPG",
    "SparkGameOpenWorld",
    "SparkGamePlatformer",
    "SparkGameRacing",
    "SparkGameRPG",
    "SparkGameRTS",
    "SparkGameVisualScript",
)
# Every prototype module that registers invalid-state rules.
STATE_VALIDATION_MODULES = (
    "SparkGame",
    "SparkGameARPG",
    "SparkGameMMO",
    "SparkGameOpenWorld",
    "SparkGamePlatformer",
    "SparkGameRacing",
    "SparkGameRPG",
    "SparkGameRTS",
    "SparkGameVisualScript",
)
SOURCE_SUFFIXES = module_content.INCLUDE_SOURCE_SUFFIXES
PRIVATE_CONSOLE_HEADERS = (
    "Utils/SparkConsole.h",
    "Utils/LogMacros.h",
)
PRIVATE_STATE_VALIDATION_HEADER = "Utils/InvalidStateDetector.h"
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
    "Utils/Validate.h",
)
FPS_PRIVATE_LOG_TOKEN = re.compile(
    r"\b(LOG_TO_CONSOLE\w*|SPARK_(?:LOG|VALIDATE|REQUIRE|ENSURE|TRACE)(?:_\w+)?)\b"
)


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


def _state_validation_violations_in_text(relative: str, text: str) -> list[str]:
    """Scan one source payload for the private detector, with comments and literals ignored."""
    findings: list[str] = []
    code, code_without_literals = module_content._lex_cpp(text)
    for match in PRIVATE_INCLUDE_PATTERN.finditer(code):
        header = match.group(1) or match.group(2)
        if header.endswith(PRIVATE_STATE_VALIDATION_HEADER):
            findings.append(f"{relative}: private state-validation header {header}")
    if re.search(r"\bInvalidStateDetector\b", code_without_literals):
        findings.append(f"{relative}: InvalidStateDetector reference")
    return findings


def _violations(module_root: Path, scan=_violations_in_text) -> list[str]:
    """Return source-boundary violations for a module."""
    source = module_root / "Source"
    findings: list[str] = []
    for path in sorted(source.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            findings.extend(
                scan(
                    path.relative_to(module_root).as_posix(),
                    path.read_text(encoding="utf-8", errors="replace"),
                )
            )
    return findings


class ModuleConsoleBoundaryTests(unittest.TestCase):
    def test_module_source_trees_are_present_and_nonempty(self) -> None:
        for name in sorted(set(MODULES) | set(STATE_VALIDATION_MODULES)):
            with self.subTest(module=name):
                source = ROOT / "GameModules" / name / "Source"
                self.assertTrue(source.is_dir())
                self.assertTrue(any(path.is_file() for path in source.rglob("*")))

    def test_prototype_modules_use_public_console_and_logging_surfaces(self) -> None:
        for name in MODULES:
            with self.subTest(module=name):
                self.assertEqual([], _violations(ROOT / "GameModules" / name))

    def test_fps_sources_use_public_console_surface(self) -> None:
        # MOD-310: every SparkGameFPS source registers commands and prints through
        # IEngineContext::GetConsole() and Spark::ModuleLog, never the engine SimpleConsole.
        self.assertEqual([], _violations(ROOT / "GameModules" / "SparkGameFPS"))

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

    def test_fps_validation_cannot_reintroduce_the_private_logger(self) -> None:
        source = (
            '#include "Utils/Validate.h"\n'
            'void F() { SPARK_REQUIRE(c, false); SPARK_REQUIRE_MSG(c, false, "failed"); SPARK_TRACE_ENTER(c); }\n'
        )
        self.assertEqual(
            [
                "Source/Probe.cpp: private log header Utils/Validate.h",
                "Source/Probe.cpp: engine log macro SPARK_REQUIRE",
                "Source/Probe.cpp: engine log macro SPARK_REQUIRE_MSG",
                "Source/Probe.cpp: engine log macro SPARK_TRACE_ENTER",
            ],
            _fps_logging_violations_in_text("Source/Probe.cpp", source),
        )
    def test_prototype_modules_use_public_state_validation_surface(self) -> None:
        for name in STATE_VALIDATION_MODULES:
            with self.subTest(module=name):
                self.assertEqual(
                    [], _violations(ROOT / "GameModules" / name, _state_validation_violations_in_text)
                )

    def test_state_validation_modules_register_rules_through_the_sdk(self) -> None:
        # The boundary scan passes vacuously for a module that dropped its rules; each listed
        # module must still reach the SDK registry.
        for name in STATE_VALIDATION_MODULES:
            with self.subTest(module=name):
                main = ROOT / "GameModules" / name / "Source" / "Core" / "Main.cpp"
                _, code = module_content._lex_cpp(main.read_text(encoding="utf-8", errors="replace"))
                for pattern in (r"GetStateValidation\(\)", r"->AddRule\(", r"->RemoveRulesByCategory\("):
                    self.assertIsNotNone(re.search(pattern, code), f"{name} Core/Main.cpp never calls {pattern}")

    def test_private_state_validation_include_mutation_fails_by_header_name(self) -> None:
        source = ROOT / "GameModules" / "SparkGameRTS" / "Source" / "Core" / "Main.cpp"
        for spelling in ("Utils/InvalidStateDetector.h", "../../../../SparkEngine/Source/Utils/InvalidStateDetector.h"):
            with self.subTest(spelling=spelling):
                mutated = f'#include "{spelling}"\n' + source.read_text(encoding="utf-8")
                self.assertIn(
                    f"Source/Core/Main.cpp: private state-validation header {spelling}",
                    _state_validation_violations_in_text("Source/Core/Main.cpp", mutated),
                )

    def test_detector_singleton_mutation_fails_by_symbol(self) -> None:
        source = ROOT / "GameModules" / "SparkGameRTS" / "Source" / "Core" / "Main.cpp"
        mutated = (
            'void UsesLegacyDetector() { Spark::InvalidStateDetector::GetInstance().RemoveRulesByCategory("RTS"); }\n'
            + source.read_text(encoding="utf-8")
        )
        self.assertIn(
            "Source/Core/Main.cpp: InvalidStateDetector reference",
            _state_validation_violations_in_text("Source/Core/Main.cpp", mutated),
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
        detector = (
            '// #include "Utils/InvalidStateDetector.h"\n'
            'const char* text = "InvalidStateDetector::GetInstance()";\n'
            "/* InvalidStateDetector */\n"
        )
        self.assertEqual([], _state_validation_violations_in_text("Source/Boundary.cpp", detector))


if __name__ == "__main__":
    unittest.main()
