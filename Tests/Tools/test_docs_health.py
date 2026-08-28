#!/usr/bin/env python3
"""Hostile fixtures for the DOC-410 documentation evidence contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path, PurePosixPath
from types import SimpleNamespace
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import docs_contract
import docs_currentness
import common as site_common
import generate as site_generate
import validate_docs_links as links


EXACT_SHA = "a" * 40
COMMITTED_AT = "2026-08-28T00:00:00Z"


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def tree_hashes(root: Path) -> dict[str, str]:
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in root.rglob("*")
        if path.is_file()
    }


class MiniContract:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="docs-contract-fixture-")
        self.root = Path(self.temporary.name)
        self.contract = self.root / "docs" / "generated-docs-manifest.json"
        self.tracked = self.root / ".tracked"
        payload = {
            "schemaVersion": 1,
            "sourceContract": {
                "includeRoots": ["SparkEngine"],
                "extensions": [".h", ".cpp"],
                "headerExtensions": [".h"],
                "excludePrefixes": ["ThirdParty/"],
            },
            "generators": [
                {
                    "id": "api-docs",
                    "script": "generate-api-docs.sh",
                    "mode": "generate",
                    "outputs": [{"path": "docs/api", "tracked": False, "tree": True}],
                }
            ],
        }
        write(self.contract, json.dumps(payload))
        self.header = self.root / "SparkEngine" / "Fixture.h"
        write(
            self.header,
            """/*
class CommentOnly {
};
*/
class SPARK_API ExportedFixture final {
};
class FooBar {
};
#define IDC_SparkEngine 100
#define IDD_SparkEngine_DIALOG 101
#define IDI_SparkEngine 102
""",
        )
        self.tracked.write_bytes(b"SparkEngine/Fixture.h\0")
        self.environment = mock.patch.dict(
            os.environ,
            {
                "SPARK_DOC_TRACKED_PATHS": str(self.tracked),
                "SPARKENGINE_DOC_SOURCE_SHA": EXACT_SHA,
                "SPARKENGINE_DOC_SOURCE_COMMITTED_AT": COMMITTED_AT,
            },
            clear=False,
        )
        self.globals = mock.patch.multiple(
            docs_contract,
            REPO_ROOT=self.root,
            CONTRACT_PATH=self.contract,
        )

    def __enter__(self) -> "MiniContract":
        self.environment.start()
        self.globals.start()
        return self

    def __exit__(self, *args: object) -> None:
        self.globals.stop()
        self.environment.stop()
        self.temporary.cleanup()


class DocsGenerationHostileTests(unittest.TestCase):
    def test_duplicate_json_members_are_rejected_by_both_contract_readers(self) -> None:
        with MiniContract() as fixture:
            source = json.loads(fixture.contract.read_text(encoding="utf-8"))
            write(
                fixture.contract,
                "{" +
                '"schemaVersion":1,"schemaVersion":1,' +
                f'"sourceContract":{json.dumps(source["sourceContract"])},' +
                f'"generators":{json.dumps(source["generators"])}' +
                "}",
            )
            with self.assertRaises(docs_contract.ContractError):
                docs_contract.load_contract()
            with self.assertRaises(site_common.SiteDataError):
                site_common.load_json(fixture.contract)

    def test_json_parser_rejects_nonfinite_and_excessive_nesting(self) -> None:
        with tempfile.TemporaryDirectory(prefix="docs-json-hostile-") as directory:
            root = Path(directory)
            path = root / "input.json"
            write(path, '{"value":NaN}')
            with self.assertRaises(docs_contract.ContractError):
                docs_contract.load_bounded_json(path, label="hostile JSON")

            write(
                path,
                "[" * (docs_contract.MAX_JSON_DEPTH + 1)
                + "0"
                + "]" * (docs_contract.MAX_JSON_DEPTH + 1),
            )
            with self.assertRaises(docs_contract.ContractError):
                docs_contract.load_bounded_json(path, label="hostile JSON")

    def test_hardlinked_source_is_rejected_before_scan(self) -> None:
        with MiniContract() as fixture:
            external = fixture.root / "external.h"
            write(external, "class External {}\n")
            fixture.header.unlink()
            try:
                os.link(external, fixture.header)
            except OSError as error:
                self.skipTest(f"hardlinks unavailable: {error}")
            with self.assertRaises(docs_contract.ContractError):
                docs_contract.source_inventory_snapshot()

    def test_reparse_source_directory_is_rejected_before_scan(self) -> None:
        with MiniContract() as fixture:
            external = fixture.root / "external"
            write(external / "Escaped.h", "class Escaped {}\n")
            junction = fixture.root / "SparkEngine" / "Junction"
            try:
                os.symlink(external, junction, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlinks unavailable: {error}")
            fixture.tracked.write_bytes(b"SparkEngine/Junction/Escaped.h\0")
            with self.assertRaises(docs_contract.ContractError):
                docs_contract.source_inventory_snapshot()

    def test_source_replacement_between_identity_and_open_is_rejected(self) -> None:
        with MiniContract() as fixture:
            rel = PurePosixPath("SparkEngine/Fixture.h")
            expected = docs_contract.source_inventory_snapshot()[rel]
            replacement = fixture.root / "replacement.h"
            write(replacement, "class RacedExternal {}\n")
            original_open = os.open
            swapped = False

            def race_open(path: str | bytes | os.PathLike[str] | os.PathLike[bytes], flags: int, *args: object) -> int:
                nonlocal swapped
                if not swapped and Path(path) == fixture.header:
                    swapped = True
                    os.replace(replacement, fixture.header)
                return original_open(path, flags, *args)

            with mock.patch.object(docs_contract.os, "open", side_effect=race_open):
                with self.assertRaises(docs_contract.ContractError):
                    docs_contract.read_source(rel, expected)

    def test_currentness_snapshot_rejects_source_replacement_race(self) -> None:
        with MiniContract() as fixture:
            rel = PurePosixPath("SparkEngine/Fixture.h")
            replacement = fixture.root / "replacement.h"
            write(replacement, "class RacedExternal {}\n")
            original_open = os.open
            swapped = False

            def race_open(path: str | bytes | os.PathLike[str] | os.PathLike[bytes], flags: int, *args: object) -> int:
                nonlocal swapped
                if not swapped and Path(path) == fixture.header:
                    swapped = True
                    os.replace(replacement, fixture.header)
                return original_open(path, flags, *args)

            with mock.patch.object(docs_currentness, "REPO_ROOT", fixture.root):
                with mock.patch.object(docs_contract.os, "open", side_effect=race_open):
                    with self.assertRaises(docs_currentness.CurrentnessError):
                        docs_currentness.copy_snapshot(
                            fixture.root / "snapshot",
                            [rel.as_posix()],
                            {rel.as_posix(): "100644"},
                        )

    def test_generated_tree_hardlink_and_over_cap_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="docs-generated-hostile-") as directory:
            root = Path(directory)
            api = root / "api"
            external = root / "external.md"
            write(external, "external\n")
            api.mkdir()
            try:
                os.link(external, api / "README.md")
            except OSError as error:
                self.skipTest(f"hardlinks unavailable: {error}")
            with self.assertRaises(docs_contract.ContractError):
                docs_contract.generated_tree_snapshot(api)

            (api / "README.md").unlink()
            write(api / "README.md", "one\n")
            write(api / "second.md", "two\n")
            write(api / "third.md", "three\n")
            with mock.patch.object(docs_contract, "MAX_GENERATED_FILES", 2):
                errors = docs_contract.validate_api_manifest(api)
            self.assertTrue(any("resource bounds" in error for error in errors), errors)

    def test_api_generation_environment_cannot_inherit_output_overrides(self) -> None:
        api_root = Path(tempfile.gettempdir()) / "fixed-api-root"
        with mock.patch.dict(
            os.environ,
            {
                "SPARK_DOC_API_OUTPUT_DIR": "attacker-api",
                "SPARK_FILE_TREE_OUTPUT": "attacker-tree",
                "SPARK_WIKI_DIR": "attacker-wiki",
            },
            clear=False,
        ):
            environment = site_generate.api_generation_environment(EXACT_SHA, COMMITTED_AT, api_root)
        self.assertEqual(environment["SPARK_DOC_API_OUTPUT_DIR"], str(api_root))
        self.assertNotIn("SPARK_FILE_TREE_OUTPUT", environment)
        self.assertNotIn("SPARK_WIKI_DIR", environment)

    def test_bounded_process_timeout_terminates_descendants_promptly(self) -> None:
        child = (
            "import subprocess,sys,time; "
            "subprocess.Popen([sys.executable,'-c','import time; time.sleep(10)']); "
            "time.sleep(10)"
        )
        started = time.monotonic()
        with self.assertRaises(site_generate.SiteDataError):
            site_generate.run_bounded_process(
                [sys.executable, "-c", child],
                cwd=REPO_ROOT,
                environment=os.environ.copy(),
                timeout=1,
                label="hostile timeout fixture",
            )
        self.assertLess(time.monotonic() - started, 5.0)

    def test_bounded_process_timeout_prevents_descendant_post_timeout_write(self) -> None:
        with tempfile.TemporaryDirectory(prefix="docs-timeout-tree-") as directory:
            marker = Path(directory) / "escaped.txt"
            grandchild = (
                "import pathlib,time; "
                "time.sleep(2); "
                f"pathlib.Path({str(marker)!r}).write_text('escaped', encoding='utf-8')"
            )
            parent = (
                "import subprocess,sys,time; "
                f"subprocess.Popen([sys.executable, '-c', {grandchild!r}]); "
                "time.sleep(10)"
            )
            with self.assertRaises(site_generate.SiteDataError):
                site_generate.run_bounded_process(
                    [sys.executable, "-c", parent],
                    cwd=REPO_ROOT,
                    environment=os.environ.copy(),
                    timeout=0.5,
                    label="hostile descendant timeout fixture",
                )
            time.sleep(2.5)
            self.assertFalse(marker.exists(), "timed-out descendant wrote after the process boundary")

    def test_opened_identity_accepts_windows_handle_permission_projection_only(self) -> None:
        path_identity = docs_contract.FileIdentity(
            device=1, inode=2, mode=stat.S_IFREG | 0o777, size=3,
            mtime_ns=4, ctime_ns=5, nlink=1, attributes=32,
        )
        handle_identity = docs_contract.FileIdentity(
            device=1, inode=2, mode=stat.S_IFREG | 0o666, size=3,
            mtime_ns=4, ctime_ns=999, nlink=1, attributes=32,
        )
        self.assertTrue(docs_contract.opened_identity_matches(path_identity, handle_identity))
        self.assertFalse(
            docs_contract.opened_identity_matches(
                path_identity,
                docs_contract.FileIdentity(
                    device=1, inode=99, mode=stat.S_IFREG | 0o666, size=3,
                    mtime_ns=4, ctime_ns=999, nlink=1, attributes=32,
                ),
            )
        )

    def test_newer_readme_cannot_hide_stale_source_or_missing_page(self) -> None:
        with MiniContract() as fixture:
            api = fixture.root / "docs" / "api"
            wiki = fixture.root / "wiki"
            docs_contract.generate_api(api)
            docs_contract.generate_indexes(api, wiki)
            docs_contract.generate_file_tree(wiki / "reference" / "File-Tree.md")
            readme = api / "README.md"
            future = time.time() + 86400
            os.utime(readme, (future, future))
            write(fixture.header, "#define IDC_SparkEngine 100\n")
            errors = docs_contract.validate_source_contract(api, wiki)
            self.assertTrue(
                any("symbol TSV differs" in error or "index" in error for error in errors),
                errors,
            )
            generated_page = api / "SparkEngine" / "Fixture.md"
            generated_page.unlink()
            errors = docs_contract.validate_source_contract(api, wiki)
            self.assertTrue(any("manifest" in error.lower() for error in errors), errors)

    def test_two_isolated_api_generations_are_byte_identical(self) -> None:
        with MiniContract() as fixture:
            first = fixture.root / "first"
            second = fixture.root / "second"
            docs_contract.generate_api(first)
            docs_contract.generate_api(second)
            self.assertEqual(tree_hashes(first), tree_hashes(second))

    def test_comment_export_substring_and_macro_tokens_are_exact(self) -> None:
        rel = PurePosixPath("SparkEngine/Fixture.h")
        source = """/*
class Fabricated {
};
*/
class SPARK_API ExportedFixture final {};
class FooBar {};
#define IDC_SparkEngine 100
#define IDD_SparkEngine_DIALOG 101
#define IDI_SparkEngine 102
"""
        symbols = docs_contract.extract_symbols(rel, source)
        names = {symbol.name for symbol in symbols}
        self.assertIn("ExportedFixture", names)
        self.assertIn("FooBar", names)
        self.assertNotIn("Foo", names)
        self.assertNotIn("Fabricated", names)
        self.assertTrue(
            {"IDC_SparkEngine", "IDD_SparkEngine_DIALOG", "IDI_SparkEngine"} <= names
        )
        self.assertFalse({"IDC_S", "IDD_S", "IDI_S"} & names)

    def test_missing_generator_cannot_report_pass_or_zero_failures(self) -> None:
        with tempfile.TemporaryDirectory(prefix="health-fixture-") as directory:
            root = Path(directory)
            results = root / "results.tsv"
            rows = [
                f"{generator}\tcurrent\tok"
                for generator in docs_currentness.REQUIRED_GENERATORS[:-1]
            ]
            write(results, "\n".join(rows) + "\n")
            output = root / "health.json"
            args = SimpleNamespace(
                source_sha=EXACT_SHA,
                source_committed_at=COMMITTED_AT,
                started_at=COMMITTED_AT,
                mode="update",
                results=results,
                output=output,
                exit_code=0,
            )
            docs_currentness.write_health(args)
            health = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual("fail", health["overall"])
            self.assertEqual(1, health["failures"])
            self.assertEqual("missing", health["results"][-1]["status"])

    def test_health_rejects_duplicate_generator_rows(self) -> None:
        with tempfile.TemporaryDirectory(prefix="health-duplicate-") as directory:
            root = Path(directory)
            results = root / "results.tsv"
            generator = docs_currentness.REQUIRED_GENERATORS[0]
            write(results, f"{generator}\tcurrent\tok\n{generator}\tcurrent\tok\n")
            args = SimpleNamespace(
                source_sha=EXACT_SHA,
                source_committed_at=COMMITTED_AT,
                started_at=COMMITTED_AT,
                mode="update",
                results=results,
                output=root / "health.json",
                exit_code=0,
            )
            with self.assertRaises(docs_currentness.CurrentnessError):
                docs_currentness.write_health(args)

    def test_nonzero_master_exit_cannot_report_zero_failures(self) -> None:
        with tempfile.TemporaryDirectory(prefix="health-exit-") as directory:
            root = Path(directory)
            results = root / "results.tsv"
            write(
                results,
                "".join(
                    f"{generator}\tcurrent\tok\n"
                    for generator in docs_currentness.REQUIRED_GENERATORS
                ),
            )
            output = root / "health.json"
            args = SimpleNamespace(
                source_sha=EXACT_SHA,
                source_committed_at=COMMITTED_AT,
                started_at=COMMITTED_AT,
                mode="update",
                results=results,
                output=output,
                exit_code=7,
            )
            self.assertFalse(docs_currentness.write_health(args))
            health = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual("fail", health["overall"])
            self.assertEqual(1, health["failures"])
            self.assertEqual(7, health["exitCode"])
            self.assertEqual("failed", health["results"][-1]["status"])

    def test_failed_result_forces_nonzero_health_exit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="health-result-") as directory:
            root = Path(directory)
            results = root / "results.tsv"
            rows = [
                f"{generator}\t{'failed' if index == 0 else 'current'}\tresult\n"
                for index, generator in enumerate(docs_currentness.REQUIRED_GENERATORS)
            ]
            write(results, "".join(rows))
            output = root / "health.json"
            args = SimpleNamespace(
                source_sha=EXACT_SHA,
                source_committed_at=COMMITTED_AT,
                started_at=COMMITTED_AT,
                mode="update",
                results=results,
                output=output,
                exit_code=0,
            )
            self.assertFalse(docs_currentness.write_health(args))
            health = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual("fail", health["overall"])
            self.assertEqual(1, health["failures"])
            self.assertEqual(1, health["exitCode"])


class LinkFixture:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="docs-link-fixture-")
        self.root = Path(self.temporary.name)
        self.docs = self.root / "docs"
        self.api = self.docs / "api"
        write(self.docs / "Guide.md", "# Guide\n")
        write(self.api / "README.md", "# API Home\n")
        self.refresh_manifest()
        self.catalog = {
            "include": {
                "rootDocuments": [],
                "recursiveMarkdownRoots": ["docs"],
            },
            "excludePrefixes": [],
            "excludePaths": [],
            "routeOverrides": {},
        }
        self.patch = mock.patch.object(links, "REPO_ROOT", self.root)

    def refresh_manifest(self) -> None:
        rows = []
        total = 0
        for path in sorted(self.api.rglob("*")):
            if not path.is_file() or path.name == ".manifest.json":
                continue
            payload = path.read_bytes()
            total += len(payload)
            rows.append(
                {
                    "path": path.relative_to(self.api).as_posix(),
                    "bytes": len(payload),
                    "sha256": hashlib.sha256(payload).hexdigest(),
                }
            )
        write(
            self.api / ".manifest.json",
            json.dumps(
                {
                    "schemaVersion": 1,
                    "sourceCommit": EXACT_SHA,
                    "files": rows,
                    "fileCount": len(rows),
                    "totalBytes": total,
                }
            ),
        )

    def __enter__(self) -> "LinkFixture":
        self.patch.start()
        return self

    def __exit__(self, *args: object) -> None:
        self.patch.stop()
        self.temporary.cleanup()


class DocsLinksHostileTests(unittest.TestCase):
    def test_reference_style_missing_target_is_rejected(self) -> None:
        with LinkFixture() as fixture:
            write(
                fixture.docs / "Guide.md",
                "# Guide\n\n[missing][ref]\n\n[ref]: missing.md\n",
            )
            errors = links.validate_docs_links(
                fixture.catalog,
                generated_root=fixture.api,
                source_sha=EXACT_SHA,
            )
            self.assertTrue(any("target does not exist" in error["error"] for error in errors), errors)

    def test_undefined_reference_is_rejected(self) -> None:
        with LinkFixture() as fixture:
            write(fixture.docs / "Guide.md", "# Guide\n\n[missing][undefined]\n")
            errors = links.validate_docs_links(
                fixture.catalog,
                generated_root=fixture.api,
                source_sha=EXACT_SHA,
            )
            self.assertTrue(any("no definition" in error["error"] for error in errors), errors)

    def test_nonexistent_generated_target_and_anchor_are_rejected(self) -> None:
        with LinkFixture() as fixture:
            write(
                fixture.docs / "Guide.md",
                "# Guide\n\n"
                "[missing](api/Missing.md)\n"
                "[anchor](api/README.md#not-there)\n",
            )
            errors = links.validate_docs_links(
                fixture.catalog,
                generated_root=fixture.api,
                source_sha=EXACT_SHA,
            )
            rendered = "\n".join(error["error"] for error in errors)
            self.assertIn("manifest", rendered)
            self.assertIn("heading anchor", rendered)

    def test_reference_inline_autolink_and_html_href_resolve(self) -> None:
        with LinkFixture() as fixture:
            write(fixture.docs / "Target.md", "# Target\n\n## Exact Anchor\n")
            write(
                fixture.docs / "Guide.md",
                "# Guide\n\n"
                "[inline](Target.md#exact-anchor)\n"
                "[reference][target]\n"
                "<Target.md#exact-anchor>\n"
                "<a href=\"Target.md#exact-anchor\">HTML</a>\n\n"
                "[target]: Target.md#exact-anchor\n",
            )
            fixture.refresh_manifest()
            errors = links.validate_docs_links(
                fixture.catalog,
                generated_root=fixture.api,
                source_sha=EXACT_SHA,
            )
            self.assertEqual([], errors)

    def test_github_anchors_are_per_document_and_inline_code_is_ignored(self) -> None:
        with LinkFixture() as fixture:
            write(
                fixture.docs / "Guide.md",
                "# Guide — One\n\n[self](#guide--one)\n\n`<windows.h>`\n",
            )
            write(
                fixture.docs / "Other.md",
                "# Other — co_await\n\n[self](#other--co_await)\n",
            )
            errors = links.validate_docs_links(
                fixture.catalog,
                generated_root=fixture.api,
                source_sha=EXACT_SHA,
            )
            self.assertEqual([], errors)

    def test_route_case_collision_is_rejected(self) -> None:
        with LinkFixture() as fixture:
            write(fixture.docs / "Other.md", "# Other\n")
            fixture.catalog["routeOverrides"] = {
                "docs/Guide.md": "same-route",
                "docs/Other.md": "same-route",
            }
            errors = links.validate_docs_routes(fixture.catalog)
            self.assertTrue(any("collision" in error["error"] for error in errors), errors)

    def test_repository_escape_is_rejected(self) -> None:
        with LinkFixture() as fixture:
            write(fixture.docs / "Guide.md", "# Guide\n\n[escape](../../outside.md)\n")
            errors = links.validate_docs_links(
                fixture.catalog,
                generated_root=fixture.api,
                source_sha=EXACT_SHA,
            )
            self.assertTrue(any("escapes repository" in error["error"] for error in errors), errors)


class RepositoryEvidenceTests(unittest.TestCase):
    def test_manifest_declares_every_generator_exactly_once(self) -> None:
        contract = docs_currentness.load_contract()
        ids = tuple(row["id"] for row in contract["generators"])
        self.assertEqual(docs_currentness.REQUIRED_GENERATORS, ids)

    def test_check_paths_are_content_based_and_read_only(self) -> None:
        scripts = {
            name: (REPO_ROOT / "docs" / name).read_text(encoding="utf-8")
            for name in (
                "generate-api-docs.sh",
                "generate-symbol-index.sh",
                "generate-file-tree.sh",
                "generate-class-hierarchy.sh",
            )
        }
        for name, content in scripts.items():
            with self.subTest(script=name):
                self.assertNotIn("-newer", content)
                self.assertRegex(content, r"\b(?:cmp|diff)\b")
        sync = (REPO_ROOT / "docs" / "sync-wiki.sh").read_text(encoding="utf-8")
        self.assertIn("SPARK_WIKI_DIR", sync)
        self.assertNotIn("Restore originals", sync)

    def test_generated_tree_covers_exact_inventory_and_gateway_loc(self) -> None:
        errors = docs_contract.validate_source_contract(
            REPO_ROOT / "docs" / "api",
            REPO_ROOT / "wiki",
        )
        self.assertEqual([], errors)
        gateway = REPO_ROOT / "Tests" / "TestGatewaySecurity.cpp"
        actual_loc = len(gateway.read_text(encoding="utf-8").splitlines())
        self.assertEqual(475, actual_loc)
        tree = (REPO_ROOT / "wiki" / "reference" / "File-Tree.md").read_text(encoding="utf-8")
        self.assertIn(
            f"(../../Tests/TestGatewaySecurity.cpp) - {actual_loc} LOC",
            tree,
        )

    def test_mixed_case_macros_have_no_fabricated_rows(self) -> None:
        rows = docs_contract.load_symbols(REPO_ROOT / "docs" / "api" / ".symbols.tsv")
        names = {row.name for row in rows}
        self.assertTrue(
            {"IDC_SparkEngine", "IDD_SparkEngine_DIALOG", "IDI_SparkEngine"} <= names
        )
        self.assertFalse({"IDC_S", "IDD_S", "IDI_S"} & names)


if __name__ == "__main__":
    unittest.main()
