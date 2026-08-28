#!/usr/bin/env python3
"""Hostile fixtures for the DOC-410 documentation evidence contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
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
