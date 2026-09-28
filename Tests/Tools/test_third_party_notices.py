#!/usr/bin/env python3
"""Tests for tools/governance/generate_third_party_notices.py (GOV-400).

The repository cases fail when a locked ThirdParty component has no license
notice on disk, when a locked container is missing from the generated file, or
when the committed THIRD_PARTY_NOTICES is stale. The fixture cases prove each
detection path can actually fire, so a clean repository result is not the
vacuous output of a check that stopped checking.
"""

from __future__ import annotations

import importlib.util
import io
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_PATH = REPO_ROOT / "tools" / "governance" / "generate_third_party_notices.py"

_spec = importlib.util.spec_from_file_location("generate_third_party_notices", TOOL_PATH)
notices = importlib.util.module_from_spec(_spec)
assert _spec.loader is not None
sys.modules[_spec.name] = notices
_spec.loader.exec_module(notices)

MIT_TEXT = "MIT License\n\nCopyright (c) 2026 Fixture Author\n\nPermission is hereby granted.\n"

FIXTURE_MANIFEST = """# fixture
_spark_manifest_gitlink_revision("ThirdParty/Sub/lib" _spark_sub_revision)
set(SPARK_THIRDPARTY_AUDIT_ENTRIES
    "Alpha|https://example.invalid/alpha|v1|MIT|ThirdParty/Alpha|alpha.h|M|F|WARN|ThirdParty/Alpha/LICENSE"
    "Sub|https://example.invalid/sub.git|${_spark_sub_revision}|zlib|ThirdParty/Sub/lib|x.h|M|F|WARN|ThirdParty/Licenses/sub-LICENSE.txt"
)
"""

FIXTURE_SUPPLY_CHAIN = {
    "managed_vendored_dirs": ["ThirdParty/Alpha"],
    "submodule_gitlinks": {"ThirdParty/Sub/lib": "a" * 40},
    "project_owned_dirs": {"ThirdParty/Licenses": "first-party notice copies"},
}


def _run_main(*args: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = notices.main(list(args))
    return code, out.getvalue(), err.getvalue()


def _repo_read(rel: str) -> bytes | None:
    path = REPO_ROOT / rel
    return path.read_bytes() if path.is_file() else None


def _sfnt(names: dict[int, str]) -> bytes:
    """A minimal sfnt holding only a Windows-English name table."""
    strings = b""
    records = b""
    for name_id, text in sorted(names.items()):
        raw = text.encode("utf-16-be")
        records += struct.pack(">6H", 3, 1, 0x409, name_id, len(raw), len(strings))
        strings += raw
    table = struct.pack(">HHH", 0, len(names), 6 + len(records)) + records + strings
    header = struct.pack(">IHHHH", 0x00010000, 1, 16, 0, 0)
    directory = struct.pack(">4sIII", b"name", 0, len(header) + 16, len(table))
    return header + directory + table


FIXTURE_COPYRIGHT = "Copyright 2026 Fixture Foundry"
OFL_DESCRIPTION = "This Font Software is licensed under the SIL Open Font License, Version 1.1."
OFL_TEXT = (
    f"{FIXTURE_COPYRIGHT}\n\n{OFL_DESCRIPTION}\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy of the Font\n"
    "Software, to use, study, copy, merge, embed, modify, redistribute, and sell copies.\n"
)


def _font_entry(**overrides: str) -> dict[str, str]:
    entry = {
        "family": "Fixture Sans",
        "version": "1.0",
        "license": "OFL-1.1",
        "copyright": FIXTURE_COPYRIGHT,
        "license_file": "OFL-1.1-Fixture.txt",
        "license_source": "https://example.invalid/fixture/OFL.txt",
    }
    entry.update(overrides)
    return entry


def _font_manifest(**overrides: str) -> str:
    return json.dumps({"schema": 1, "fonts": {"A.ttf": _font_entry(**overrides)}})


class RepositoryNoticesTests(unittest.TestCase):
    """Checks against the real checkout."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.components, cls.supply_chain, cls.fonts = notices.load(REPO_ROOT)
        cls.text = notices.render(cls.components, cls.supply_chain, cls.fonts)

    def test_every_locked_container_is_a_component(self) -> None:
        expected = set(self.supply_chain["managed_vendored_dirs"]) | set(self.supply_chain["submodule_gitlinks"])
        self.assertGreater(len(expected), 0, "supply-chain.lock lists no containers; the check would be vacuous")
        self.assertEqual({c.path for c in self.components}, expected)

    def test_every_vendored_component_has_a_notice_on_disk(self) -> None:
        missing = [c.path for c in self.components if not c.has_notice]
        self.assertEqual(missing, [], f"locked components without any license notice on disk: {missing}")
        for component in self.components:
            self.assertIsNotNone(component.entry, f"{component.path} has no dependencies.lock entry")
            for path, text in component.notices:
                self.assertTrue(text.strip(), f"{path} is empty")
                self.assertIn(text.rstrip("\n"), self.text, f"{path} text is not reproduced verbatim")

    def test_generation_is_deterministic(self) -> None:
        first, _ = notices.generate(REPO_ROOT)
        second, _ = notices.generate(REPO_ROOT)
        self.assertEqual(first.encode("utf-8"), second.encode("utf-8"))

    def test_every_font_outside_thirdparty_has_license_text(self) -> None:
        tracked = notices.git_tracked_files(REPO_ROOT)
        fonts = [rel for rel in tracked if notices._is_font(rel) and not rel.startswith("ThirdParty/")]
        self.assertTrue(fonts, "no tracked fonts outside ThirdParty/; the check would be vacuous")
        self.assertEqual(self.fonts.findings, [])
        self.assertEqual(sorted(f.font for f in self.fonts.notices), sorted(fonts))
        self.assertNotIn("no license file on disk for SparkEditor/Fonts", self.text)
        self.assertTrue(self.fonts.texts)
        for rel, text in self.fonts.texts.items():
            self.assertIn(text.rstrip("\n"), self.text, f"{rel} text is not reproduced verbatim")

    def test_removing_an_editor_font_license_file_is_reported(self) -> None:
        tracked = [rel for rel in notices.git_tracked_files(REPO_ROOT) if not rel.endswith("/Apache-2.0-Roboto.txt")]
        inventory = notices.font_inventory(tracked, _repo_read)
        self.assertEqual(
            sorted(f.split(":", 1)[0] for f in inventory.findings),
            ["SparkEditor/Fonts/Roboto-Bold.ttf", "SparkEditor/Fonts/Roboto-Regular.ttf"],
        )

    def test_committed_file_is_current(self) -> None:
        committed = REPO_ROOT / notices.OUTPUT_NAME
        self.assertTrue(committed.is_file(), "THIRD_PARTY_NOTICES is not committed")
        on_disk = committed.read_bytes().decode("utf-8").replace("\r\n", "\n")
        self.assertEqual(on_disk, self.text, "THIRD_PARTY_NOTICES is stale; regenerate it")


class FixtureDetectionTests(unittest.TestCase):
    """Each detection path must fire on a constructed failure."""

    def _components(self, files: dict[str, str], tracked: list[str] | None = None, manifest: str = FIXTURE_MANIFEST):
        manifest_entries = notices.parse_manifest(manifest, FIXTURE_SUPPLY_CHAIN["submodule_gitlinks"])

        def read(rel: str) -> bytes | None:
            return files[rel].encode("utf-8") if rel in files else None

        return notices.build_components(
            Path("."), manifest_entries, FIXTURE_SUPPLY_CHAIN, sorted(tracked if tracked is not None else files), read
        )

    def test_complete_fixture_has_no_findings(self) -> None:
        components = self._components(
            {"ThirdParty/Alpha/LICENSE": MIT_TEXT, "ThirdParty/Licenses/sub-LICENSE.txt": MIT_TEXT}
        )
        self.assertEqual([c.findings for c in components], [[], []])
        self.assertEqual(components[1].entry.version, "a" * 40, "gitlink variable not substituted")

    def test_missing_notice_file_is_reported_not_guessed(self) -> None:
        components = self._components({"ThirdParty/Licenses/sub-LICENSE.txt": MIT_TEXT})
        alpha = next(c for c in components if c.path == "ThirdParty/Alpha")
        self.assertFalse(alpha.has_notice)
        self.assertTrue(any("missing on disk: ThirdParty/Alpha/LICENSE" in f for f in alpha.findings))
        rendered = notices.render(components, FIXTURE_SUPPLY_CHAIN, notices.FontInventory())
        self.assertIn("NO LICENSE TEXT AVAILABLE ON DISK FOR THIS COMPONENT.", rendered)

    def test_container_without_manifest_entry_is_reported(self) -> None:
        manifest = FIXTURE_MANIFEST.replace(
            '    "Alpha|https://example.invalid/alpha|v1|MIT|ThirdParty/Alpha|alpha.h|M|F|WARN|ThirdParty/Alpha/LICENSE"\n',
            "",
        )
        components = self._components({"ThirdParty/Licenses/sub-LICENSE.txt": MIT_TEXT}, manifest=manifest)
        alpha = next(c for c in components if c.path == "ThirdParty/Alpha")
        self.assertIsNone(alpha.entry)
        self.assertFalse(alpha.has_notice)
        self.assertTrue(alpha.findings)

    def test_stub_undeclared_license_font_and_non_spdx_are_flagged(self) -> None:
        manifest = FIXTURE_MANIFEST.replace("|v1|MIT|", "|v1|Public Domain / MIT|")
        files = {
            "ThirdParty/Alpha/LICENSE": MIT_TEXT,
            "ThirdParty/Alpha/alpha.h": "/**\n * This is a MINIMAL STUB of alpha.\n */\n",
            "ThirdParty/Alpha/Assets/LICENSE.txt": "Different terms for assets.\n",
            "ThirdParty/Alpha/Assets/Fonts/Face.ttf": "binary",
            "ThirdParty/Licenses/sub-LICENSE.txt": MIT_TEXT,
        }
        alpha = self._components(files, manifest=manifest)[0]
        joined = "\n".join(alpha.findings)
        self.assertIn("not a single SPDX identifier", joined)
        self.assertIn("repository-authored stub", joined)
        self.assertIn("undeclared license file", joined)
        self.assertIn("font file has no license file beside it", joined)
        self.assertEqual([p for p, _ in alpha.extra_notices], ["ThirdParty/Alpha/Assets/LICENSE.txt"])

    @staticmethod
    def _fonts(overrides: dict[str, bytes | str | None] | None = None, names: dict[int, str] | None = None):
        """Font inventory of a fixture tree; an override value of None removes that file."""
        tree: dict[str, bytes | str | None] = {
            "Editor/Fonts/A.ttf": _sfnt(names if names is not None else {0: FIXTURE_COPYRIGHT, 13: OFL_DESCRIPTION}),
            "Editor/Fonts/LICENSES/fonts.json": _font_manifest(),
            "Editor/Fonts/LICENSES/OFL-1.1-Fixture.txt": OFL_TEXT,
        }
        tree.update(overrides or {})
        files = {rel: value for rel, value in tree.items() if value is not None}

        def read(rel: str) -> bytes | None:
            value = files.get(rel)
            return value.encode("utf-8") if isinstance(value, str) else value

        return notices.font_inventory(sorted(files), read)

    def test_font_with_matching_license_inventory_is_covered(self) -> None:
        inventory = self._fonts()
        self.assertEqual(inventory.findings, [])
        self.assertEqual([f.font for f in inventory.notices], ["Editor/Fonts/A.ttf"])
        self.assertEqual(list(inventory.texts), ["Editor/Fonts/LICENSES/OFL-1.1-Fixture.txt"])
        rendered = notices.render([], FIXTURE_SUPPLY_CHAIN, inventory)
        self.assertIn(OFL_TEXT.rstrip("\n"), rendered)
        self.assertIn("Editor/Fonts/A.ttf: Fixture Sans 1.0 - OFL-1.1", rendered)

    def test_font_license_inventory_failures_are_reported(self) -> None:
        two_fonts = json.dumps({"schema": 1, "fonts": {"A.ttf": _font_entry(), "B.ttf": _font_entry()}})
        cases = {
            "a sibling LICENSE does not say which font it covers": (
                {"Editor/Fonts/LICENSES/fonts.json": None, "Editor/Fonts/LICENSE": OFL_TEXT},
                "no license file on disk for Editor/Fonts/A.ttf",
            ),
            "license file not tracked": (
                {"Editor/Fonts/LICENSES/OFL-1.1-Fixture.txt": None},
                "license file Editor/Fonts/LICENSES/OFL-1.1-Fixture.txt is not tracked",
            ),
            "license file without operative terms": (
                {"Editor/Fonts/LICENSES/OFL-1.1-Fixture.txt": FIXTURE_COPYRIGHT + "\n" + "x" * 300},
                "has no operative license terms",
            ),
            "recorded copyright differs from the font": (
                {"Editor/Fonts/LICENSES/fonts.json": _font_manifest(copyright="Copyright 1999 Someone Else")},
                f"is not the font's own '{FIXTURE_COPYRIGHT}'",
            ),
            "recorded license differs from the font": (
                {"Editor/Fonts/LICENSES/fonts.json": _font_manifest(license="Apache-2.0")},
                "recorded license Apache-2.0 but the font declares OFL-1.1",
            ),
            "unreadable font": ({"Editor/Fonts/A.ttf": b"wOF2 compressed"}, "cannot read the font's name table"),
            "manifest names an untracked font": (
                {"Editor/Fonts/LICENSES/fonts.json": two_fonts},
                "Editor/Fonts/LICENSES/fonts.json names untracked font B.ttf",
            ),
        }
        for label, (overrides, expected) in cases.items():
            with self.subTest(label):
                inventory = self._fonts(overrides)
                self.assertTrue(any(expected in f for f in inventory.findings), inventory.findings)
                if label != "manifest names an untracked font":
                    self.assertEqual(inventory.notices, [])

    def test_unclassified_font_license_description_needs_owner(self) -> None:
        inventory = self._fonts(names={0: FIXTURE_COPYRIGHT, 13: "Custom EULA, see vendor"})
        self.assertTrue(any("needs owner classification" in f for f in inventory.findings), inventory.findings)
        self.assertEqual(inventory.notices, [])

    def test_font_without_license_description_uses_the_recorded_license(self) -> None:
        self.assertEqual(self._fonts(names={0: FIXTURE_COPYRIGHT}).findings, [])

    def test_malformed_font_manifest_fails_closed(self) -> None:
        cases = {
            "not json": "{",
            "schema": json.dumps({"schema": 2, "fonts": {"A.ttf": _font_entry()}}),
            "missing field": json.dumps({"schema": 1, "fonts": {"A.ttf": {"license": "OFL-1.1"}}}),
            "non-SPDX license": _font_manifest(license="OFL or whatever"),
            "license file outside LICENSES/": _font_manifest(license_file="../OFL.txt"),
        }
        for label, manifest in cases.items():
            with self.subTest(label):
                with self.assertRaises(notices.NoticeInputError):
                    self._fonts({"Editor/Fonts/LICENSES/fonts.json": manifest})

    def test_font_name_table_is_read_from_the_font(self) -> None:
        names = notices.font_name_table(_sfnt({0: "Copyright X", 13: OFL_DESCRIPTION}))
        self.assertEqual(names, {0: "Copyright X", 13: OFL_DESCRIPTION})
        with self.assertRaises(ValueError):
            notices.font_name_table(b"\x00\x01\x00\x00\x00\x01")

    def test_line_endings_do_not_change_output(self) -> None:
        lf = self._components({"ThirdParty/Alpha/LICENSE": MIT_TEXT, "ThirdParty/Licenses/sub-LICENSE.txt": MIT_TEXT})
        crlf = self._components(
            {
                "ThirdParty/Alpha/LICENSE": MIT_TEXT.replace("\n", "\r\n"),
                "ThirdParty/Licenses/sub-LICENSE.txt": "﻿" + MIT_TEXT + "\n\n",
            }
        )
        self.assertEqual(
            notices.render(lf, FIXTURE_SUPPLY_CHAIN, notices.FontInventory()),
            notices.render(crlf, FIXTURE_SUPPLY_CHAIN, notices.FontInventory()),
        )

    def test_malformed_manifests_fail_closed(self) -> None:
        gitlinks = FIXTURE_SUPPLY_CHAIN["submodule_gitlinks"]
        cases = {
            "list append": FIXTURE_MANIFEST + 'list(APPEND SPARK_THIRDPARTY_AUDIT_ENTRIES "x|y")\n',
            "unresolved variable": FIXTURE_MANIFEST.replace("${_spark_sub_revision}", "${_nope}"),
            "field count": FIXTURE_MANIFEST.replace("|WARN|ThirdParty/Alpha/LICENSE", "|ThirdParty/Alpha/LICENSE"),
            "no block": "# nothing here\n",
            "unlocked gitlink": FIXTURE_MANIFEST.replace('("ThirdParty/Sub/lib"', '("ThirdParty/Other"'),
        }
        for label, manifest in cases.items():
            with self.subTest(label):
                with self.assertRaises(notices.NoticeInputError):
                    notices.parse_manifest(manifest, gitlinks)


RULES_PATH = REPO_ROOT / "cmake" / "PackageNoticeCoverageRules.json"
PACKAGE_GATE = REPO_ROOT / "cmake" / "ValidateStagedPackageNotices.cmake"
PACKAGE_VALIDATOR = REPO_ROOT / "cmake" / "ValidateStagedPackageExecutables.cmake"
AUDIT_MODULE = REPO_ROOT / "cmake" / "SparkThirdPartyAudit.cmake"

FONT_LICENSE = (
    "Copyright 2026 Fixture Type Foundry\n\n"
    "This Font Software is licensed under a fixture license used only by the\n"
    "SparkEngine notice-coverage contract test.\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
    "of the Font Software, to use, study, copy, merge, embed, modify, redistribute,\n"
    "and sell modified and unmodified copies of the Font Software.\n"
)
LIBRARY_LICENSE = MIT_TEXT + "Redistribution and use in source and binary forms is permitted.\n" * 3


def _package_notice(font_body: str | None, jolt_body: str | None, font_files: str) -> str:
    """A THIRD_PARTY_NOTICES.txt in the format cmake/SparkThirdPartyAudit.cmake writes."""
    text = (
        "SparkEngine Third-Party Notices\n================================\n\n"
        "SparkEngine includes or can link the dependencies listed below.\n\n"
        "Dependency inventory\n--------------------\n\n"
        "Jolt Physics\n  Source: https://example.invalid/jolt\n  Version: v5 (fixture; [pinned])\n"
        "  License: MIT\n  Notice files: ThirdParty/Physics/JoltPhysics/LICENSE\n"
        "  Files: Jolt/Jolt.h,Build/CMakeLists.txt\n\n"
        "Fixture Sans\n  Source: https://example.invalid/sans\n  Version: 1.0\n  License: OFL-1.1\n"
        "  Notice files: SparkEditor/Fonts/FixtureSans-LICENSE.txt\n"
        f"  Files: {font_files}\n\n"
        "Complete license and notice texts\n=================================\n\n"
    )
    if jolt_body is not None:
        text += f"----- ThirdParty/Physics/JoltPhysics/LICENSE -----\n\n{jolt_body}\n\n"
    if font_body is not None:
        text += f"----- SparkEditor/Fonts/FixtureSans-LICENSE.txt -----\n\n{font_body}\n\n"
    return text


def _write_package(root: Path, notice: str) -> None:
    files = {
        "LICENSE.txt": "fixture first-party license\n",
        "bin/EditorAssets/Fonts/FixtureSans-Regular.ttf": "fixture font bytes\n",
        "include/Jolt/Jolt.h": "#pragma once\n",
        "include/Jolt/Core/Core.h": "#pragma once\n",
        "include/SparkEngine/Core/Engine.h": "#pragma once\n",
        "include/SparkEngine/ThirdParty/angelscript.h": "#pragma once\n",
        "THIRD_PARTY_NOTICES.txt": notice,
    }
    for rel, content in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(content, encoding="utf-8")


class PackageRuleSetTests(unittest.TestCase):
    """The shared rule set is well formed and anchored to the dependency manifest."""

    def test_rules_load_and_components_are_locked_dependencies(self) -> None:
        rules = notices.load_package_rules(RULES_PATH)
        supply_chain = notices.parse_supply_chain((REPO_ROOT / notices.SUPPLY_CHAIN_PATH).read_text("utf-8"))
        manifest = notices.parse_manifest(
            (REPO_ROOT / notices.MANIFEST_PATH).read_text("utf-8"), supply_chain["submodule_gitlinks"]
        )
        names = {entry.name for entry in manifest}
        unknown = sorted({r.component for r in rules.payload if r.component and r.component not in names})
        self.assertEqual(unknown, [], "payload rules name components absent from ThirdParty/dependencies.lock")
        for rule in rules.payload:
            if rule.component is None:
                self.assertTrue(rule.first_party, f"{rule.pattern.pattern} exempts payload without a reason")

    def test_generator_font_inventory_uses_the_shared_rules(self) -> None:
        rules = notices.load_package_rules(RULES_PATH)
        self.assertTrue({".ttf", ".otf", ".woff", ".woff2"} <= rules.font_suffixes)
        tracked = [f"Editor/Fonts/A{suffix}" for suffix in sorted(rules.font_suffixes)] + ["Editor/Fonts/A.png"]
        self.assertEqual(
            notices.font_inventory(tracked, lambda rel: None).findings,
            [
                f"no license file on disk for Editor/Fonts/A{suffix} (no entry in Editor/Fonts/LICENSES/fonts.json)"
                for suffix in sorted(rules.font_suffixes)
            ],
        )

    def test_malformed_rules_fail_closed(self) -> None:
        good = json.loads(RULES_PATH.read_text("utf-8"))
        cases = {
            "schema": {**good, "schema": 2},
            "no payload rules": {**good, "payloadRules": []},
            "rule without target": {**good, "payloadRules": [{"pattern": "^include/"}]},
            "rule with both targets": {
                **good,
                "payloadRules": [{"pattern": "^include/", "component": "zstd", "firstParty": "why"}],
            },
            "bad regex": {**good, "thirdPartyRoots": ["^include/("]},
            "undotted suffix": {**good, "fontSuffixes": ["ttf"]},
        }
        for label, data in cases.items():
            with self.subTest(label):
                with self.assertRaises(notices.NoticeInputError):
                    notices.parse_package_rules(json.dumps(data))

    def test_package_validator_runs_the_notice_gate_after_required_content(self) -> None:
        text = PACKAGE_VALIDATOR.read_text("utf-8")
        required = text.find("missing required runtime content")
        gate = text.find('include("${CMAKE_CURRENT_LIST_DIR}/ValidateStagedPackageNotices.cmake")')
        self.assertGreater(required, 0)
        self.assertGreater(gate, required, "the notice gate must run in the full package validation path")
        self.assertIn("set(SPARK_PACKAGE_NOTICE_COVERAGE enforce)", text)
        self.assertNotIn("set(SPARK_PACKAGE_NOTICE_COVERAGE report)", text)


class LicenseInventoryPackageTests(unittest.TestCase):
    """LicenseInventory_*: both implementations of the gate agree on fixture packages."""

    CASES = {
        "covered_font": (_package_notice(FONT_LICENSE, LIBRARY_LICENSE, "FixtureSans-Regular.ttf"), {}, []),
        "uncovered_font": (
            _package_notice(FONT_LICENSE, LIBRARY_LICENSE, "FixtureSans-Regular.ttf"),
            {"bin/EditorAssets/Fonts/Unlisted-Bold.otf": "font\n"},
            ["bin/EditorAssets/Fonts/Unlisted-Bold.otf: font not named"],
        ),
        "font_named_without_license_text": (
            _package_notice(None, LIBRARY_LICENSE, "FixtureSans-Regular.ttf"),
            {},
            ["bin/EditorAssets/Fonts/FixtureSans-Regular.ttf: font named by 'Fixture Sans' but license text"],
        ),
        "font_named_without_terms": (
            _package_notice("Copyright 2026 Fixture. " * 12, LIBRARY_LICENSE, "FixtureSans-Regular.ttf"),
            {},
            ["bin/EditorAssets/Fonts/FixtureSans-Regular.ttf: font named by 'Fixture Sans'"],
        ),
        "payload_without_license_text": (
            _package_notice(FONT_LICENSE, None, "FixtureSans-Regular.ttf"),
            {},
            ["include/Jolt/Core/Core.h: component 'Jolt Physics'", "include/Jolt/Jolt.h: component 'Jolt Physics'"],
        ),
        "unmapped_payload": (
            _package_notice(FONT_LICENSE, LIBRARY_LICENSE, "FixtureSans-Regular.ttf"),
            {"include/SparkEngine/ThirdParty/newlib/newlib.h": "#pragma once\n"},
            ["include/SparkEngine/ThirdParty/newlib/newlib.h: third-party install path that no payload rule maps"],
        ),
    }

    def _run_case(self, notice: str, extra: dict[str, str]) -> tuple[Path, notices.PackageCoverage]:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name) / "pkg"
        _write_package(root, notice)
        for rel, content in extra.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(content, encoding="utf-8")
        return root, notices.check_package_coverage(root, notices.load_package_rules(RULES_PATH))

    def test_python_verdicts(self) -> None:
        for label, (notice, extra, expected) in self.CASES.items():
            with self.subTest(label):
                _, coverage = self._run_case(notice, extra)
                self.assertEqual(len(coverage.uncovered), len(expected), coverage.uncovered)
                for fragment, line in zip(expected, coverage.uncovered):
                    self.assertTrue(line.startswith(fragment), f"{line!r} does not start with {fragment!r}")
                if not expected:
                    self.assertEqual(coverage.font_count, 1)
                    self.assertEqual(coverage.payload_count, 3)

    def test_check_package_cli_exit_codes(self) -> None:
        notice, extra, _ = self.CASES["uncovered_font"]
        root, _ = self._run_case(notice, extra)
        code, _, err = _run_main("--check-package", str(root))
        self.assertEqual(code, 1)
        self.assertIn("Unlisted-Bold.otf", err)
        (root / "bin/EditorAssets/Fonts/Unlisted-Bold.otf").unlink()
        self.assertEqual(_run_main("--check-package", str(root))[0], 0)
        (root / "THIRD_PARTY_NOTICES.txt").write_text("not the generated format\n", encoding="utf-8")
        self.assertEqual(_run_main("--check-package", str(root))[0], 2)

    @unittest.skipUnless(shutil.which("cmake"), "cmake is required for the parity check")
    def test_cmake_gate_agrees_with_python(self) -> None:
        for label, (notice, extra, _) in self.CASES.items():
            with self.subTest(label):
                root, coverage = self._run_case(notice, extra)
                result = subprocess.run(
                    ["cmake", f"-DSPARK_PACKAGE_ROOT={root}", "-P", str(PACKAGE_GATE)],
                    capture_output=True,
                    text=True,
                    timeout=120,
                )
                flat = " ".join((result.stdout + result.stderr).split())
                self.assertEqual(result.returncode == 0, not coverage.uncovered, flat)
                for line in coverage.uncovered:
                    self.assertIn(" ".join(line.split()), flat, "CMake gate did not report the same file")

    @unittest.skipUnless(shutil.which("cmake"), "cmake is required to render the packaged notice file")
    def test_real_packaged_notice_licenses_every_entry_and_names_files(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        out = Path(tmp.name) / "THIRD_PARTY_NOTICES.txt"
        script = Path(tmp.name) / "render.cmake"
        script.write_text(
            f'include("{AUDIT_MODULE.as_posix()}")\n'
            f'spark_thirdparty_generate_notice("{(REPO_ROOT / notices.MANIFEST_PATH).as_posix()}" '
            f'"{out.as_posix()}")\n',
            encoding="utf-8",
        )
        subprocess.run(["cmake", "-P", str(script)], check=True, capture_output=True, timeout=120)
        rules = notices.load_package_rules(RULES_PATH)
        entries = notices.parse_package_notice(out.read_text("utf-8"), rules)
        self.assertGreater(len(entries), 0)
        for entry in entries:
            self.assertEqual(entry.problem, "", f"{entry.name}: {entry.problem}")
            self.assertTrue(entry.files, f"{entry.name} has no 'Files:' line")
        # The real notice covers mapped payload and every editor font the editor
        # installs (GOV-400 D8): each font is named on a 'Files:' line of an entry
        # that reproduces its license text.
        root = Path(tmp.name) / "pkg"
        (root / "include/Jolt").mkdir(parents=True)
        (root / "include/Jolt/Jolt.h").write_text("#pragma once\n", encoding="utf-8")
        (root / "bin/EditorAssets/Fonts").mkdir(parents=True)
        for font in sorted((REPO_ROOT / "SparkEditor" / "Fonts").glob("*.ttf")):
            (root / "bin/EditorAssets/Fonts" / font.name).write_bytes(b"font")
        (root / "THIRD_PARTY_NOTICES.txt").write_bytes(out.read_bytes())
        coverage = notices.check_package_coverage(root, rules)
        named = {PurePosixPath(rel).name for entry in entries for rel in entry.files}
        fonts = sorted(p.name for p in (REPO_ROOT / "SparkEditor" / "Fonts").glob("*.ttf"))
        self.assertEqual(len(fonts), 7, "SparkEditor/Fonts changed; the check must still cover every font")
        self.assertEqual(coverage.font_count, len(fonts))
        self.assertEqual(coverage.uncovered, [])
        self.assertEqual([font for font in fonts if font not in named], [])

    @unittest.skipUnless(shutil.which("cmake"), "cmake is required to render the packaged notice file")
    def test_packaged_notice_fails_closed_on_an_incomplete_font_inventory(self) -> None:
        lock = (
            "set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n"
            '    "Alpha|https://example.invalid/alpha|v1|MIT|ThirdParty/Alpha|alpha.h|M|F|WARN|'
            'ThirdParty/Alpha/LICENSE"\n)\n'
        )
        cases = {
            "font missing from the inventory": (
                {"A.ttf": _font_entry()},
                ["A.ttf", "B.ttf"],
                "has no entry for editor font B.ttf",
            ),
            "license text missing": (
                {"A.ttf": _font_entry(license_file="Missing.txt")},
                ["A.ttf"],
                "license notice file does not exist: SparkEditor/Fonts/LICENSES/Missing.txt",
            ),
            "inventory without fonts": ({}, ["A.ttf"], "'fonts' is empty or malformed"),
        }
        for label, (fonts, files, expected) in cases.items():
            with self.subTest(label):
                tmp = tempfile.TemporaryDirectory()
                self.addCleanup(tmp.cleanup)
                root = Path(tmp.name)
                (root / "ThirdParty/Alpha").mkdir(parents=True)
                (root / "ThirdParty/dependencies.lock").write_text(lock, encoding="utf-8")
                (root / "ThirdParty/Alpha/LICENSE").write_text(LIBRARY_LICENSE, encoding="utf-8")
                licenses = root / "SparkEditor/Fonts/LICENSES"
                licenses.mkdir(parents=True)
                (licenses / "OFL-1.1-Fixture.txt").write_text(FONT_LICENSE, encoding="utf-8")
                (licenses / "fonts.json").write_text(json.dumps({"schema": 1, "fonts": fonts}), encoding="utf-8")
                for name in files:
                    (root / "SparkEditor/Fonts" / name).write_bytes(b"font")
                script = root / "render.cmake"
                script.write_text(
                    f'include("{AUDIT_MODULE.as_posix()}")\n'
                    f'spark_thirdparty_generate_notice("{(root / "ThirdParty/dependencies.lock").as_posix()}" '
                    f'"{(root / "out.txt").as_posix()}")\n',
                    encoding="utf-8",
                )
                result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=120)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(expected, " ".join(result.stderr.split()))


@unittest.skipUnless(shutil.which("git"), "git is required for the end-to-end fixture")
class EndToEndTests(unittest.TestCase):
    """Drive main() against a throwaway git repository."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "ThirdParty" / "Alpha").mkdir(parents=True)
        (self.root / "ThirdParty" / "Licenses").mkdir(parents=True)
        (self.root / "ThirdParty" / "dependencies.lock").write_text(FIXTURE_MANIFEST, encoding="utf-8")
        (self.root / "ThirdParty" / "supply-chain.lock").write_text(json.dumps(FIXTURE_SUPPLY_CHAIN), encoding="utf-8")
        (self.root / "ThirdParty" / "Alpha" / "LICENSE").write_text(MIT_TEXT, encoding="utf-8")
        (self.root / "ThirdParty" / "Licenses" / "sub-LICENSE.txt").write_text(MIT_TEXT, encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_check_write_check_cycle(self) -> None:
        root = str(self.root)
        self.assertEqual(_run_main("--root", root, "--check")[0], 1, "--check must fail when the file is missing")
        self.assertEqual(_run_main("--root", root)[0], 0)
        first = (self.root / notices.OUTPUT_NAME).read_bytes()
        self.assertEqual(_run_main("--root", root)[0], 0)
        self.assertEqual((self.root / notices.OUTPUT_NAME).read_bytes(), first, "second run was not byte-identical")
        self.assertEqual(_run_main("--root", root, "--check", "--require-complete")[0], 0)

        (self.root / notices.OUTPUT_NAME).write_bytes(first + b"tampered\n")
        self.assertEqual(_run_main("--root", root, "--check")[0], 1, "--check must fail on a stale file")

    def test_require_complete_fails_when_a_notice_is_removed(self) -> None:
        (self.root / "ThirdParty" / "Alpha" / "LICENSE").unlink()
        code, _, err = _run_main("--root", str(self.root), "--require-complete")
        self.assertEqual(code, 1)
        self.assertIn("ThirdParty/Alpha", err)

    def test_require_complete_fails_on_a_font_without_license_text(self) -> None:
        (self.root / "Editor").mkdir()
        (self.root / "Editor" / "Face.ttf").write_bytes(_sfnt({0: FIXTURE_COPYRIGHT}))
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        code, _, err = _run_main("--root", str(self.root), "--require-complete")
        self.assertEqual(code, 1)
        self.assertIn("font without license text: no license file on disk for Editor/Face.ttf", err)

    def test_malformed_lock_exits_two(self) -> None:
        (self.root / "ThirdParty" / "supply-chain.lock").write_text("{not json", encoding="utf-8")
        self.assertEqual(_run_main("--root", str(self.root))[0], 2)


if __name__ == "__main__":
    unittest.main()
