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
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

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
        rendered = notices.render(components, FIXTURE_SUPPLY_CHAIN, [])
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

    def test_fonts_outside_thirdparty_are_reported(self) -> None:
        tracked = ["Editor/Fonts/A.ttf", "Other/Fonts/B.otf", "Other/Fonts/OFL.txt", "Other/Fonts/LICENSE"]
        self.assertEqual(notices.uncovered_fonts(FIXTURE_SUPPLY_CHAIN, tracked), ["Editor/Fonts/A.ttf"])

    def test_line_endings_do_not_change_output(self) -> None:
        lf = self._components({"ThirdParty/Alpha/LICENSE": MIT_TEXT, "ThirdParty/Licenses/sub-LICENSE.txt": MIT_TEXT})
        crlf = self._components(
            {
                "ThirdParty/Alpha/LICENSE": MIT_TEXT.replace("\n", "\r\n"),
                "ThirdParty/Licenses/sub-LICENSE.txt": "﻿" + MIT_TEXT + "\n\n",
            }
        )
        self.assertEqual(
            notices.render(lf, FIXTURE_SUPPLY_CHAIN, []), notices.render(crlf, FIXTURE_SUPPLY_CHAIN, [])
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

    def test_malformed_lock_exits_two(self) -> None:
        (self.root / "ThirdParty" / "supply-chain.lock").write_text("{not json", encoding="utf-8")
        self.assertEqual(_run_main("--root", str(self.root))[0], 2)


if __name__ == "__main__":
    unittest.main()
