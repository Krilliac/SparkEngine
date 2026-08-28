#!/usr/bin/env python3
"""Fail-closed tests for documentation generation health and link integrity.

Implements DOC-410 test selectors: DocsGeneration_* and DocsLinks_*.
All tests are read-only — they validate without mutating tracked files.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

from common import SITE_CONTRACT_ROOT, load_json  # noqa: E402
from validate_docs_links import (  # noqa: E402
    collect_documents,
    validate_docs_links,
    validate_docs_routes,
)


DOCS_DIR = REPO_ROOT / "docs"
UPDATE_ALL = DOCS_DIR / "update-all-docs.sh"
CATALOG_PATH = SITE_CONTRACT_ROOT / "docs-catalog.json"

GENERATOR_SCRIPTS = [
    "sync-wiki.sh",
    "generate-api-docs.sh",
    "generate-symbol-index.sh",
    "generate-file-tree.sh",
    "generate-class-hierarchy.sh",
    "generate-flowchart.sh",
    "update-codebase-stats.sh",
    "update-readme-badges.sh",
    "update-context.sh",
]


class DocsGeneration_CheckModeIdempotent(unittest.TestCase):
    """DocsGeneration: check mode must not mutate tracked files."""

    def test_api_docs_check_mode_is_readonly(self) -> None:
        """generate-api-docs.sh check must not call 'main generate'."""
        script = DOCS_DIR / "generate-api-docs.sh"
        content = script.read_text(encoding="utf-8")
        check_section = re.search(
            r"check\)(.*?)(?:;;)", content, re.DOTALL
        )
        self.assertIsNotNone(check_section, "check) case not found")
        body = check_section.group(1)
        self.assertNotIn(
            "main generate", body,
            "check mode must not invoke generation — it should only report staleness",
        )
        self.assertNotIn(
            "echo.*checksum", body.replace(" ", ""),
            "check mode must not write checksum files",
        )


class DocsGeneration_HealthJsonEmitted(unittest.TestCase):
    """DocsGeneration: structured health JSON is emitted by the master script."""

    def test_update_all_writes_health_json(self) -> None:
        content = UPDATE_ALL.read_text(encoding="utf-8")
        self.assertIn("write_health_json", content)
        self.assertIn(".health.json", content)

    def test_health_json_includes_per_script_results(self) -> None:
        content = UPDATE_ALL.read_text(encoding="utf-8")
        self.assertIn("HEALTH_RESULTS", content)
        for status in ("current", "stale", "failed", "missing"):
            self.assertIn(
                status, content,
                f"health result status {status!r} not found in master script",
            )


class DocsGeneration_AllScriptsHaveCheckMode(unittest.TestCase):
    """DocsGeneration: every orchestrated script supports check mode."""

    def test_every_generator_script_has_check_case(self) -> None:
        for script_name in GENERATOR_SCRIPTS:
            script = DOCS_DIR / script_name
            with self.subTest(script=script_name):
                self.assertTrue(script.is_file(), f"{script_name} does not exist")
                content = script.read_text(encoding="utf-8")
                self.assertRegex(
                    content, r"check\)",
                    f"{script_name} has no check) case",
                )


class DocsGeneration_MasterScriptOrchestration(unittest.TestCase):
    """DocsGeneration: master script orchestrates all generators."""

    def test_master_script_runs_all_generators(self) -> None:
        content = UPDATE_ALL.read_text(encoding="utf-8")
        for script_name in GENERATOR_SCRIPTS:
            with self.subTest(script=script_name):
                self.assertIn(script_name, content)

    def test_check_mode_loops_all_generators(self) -> None:
        content = UPDATE_ALL.read_text(encoding="utf-8")
        check_section = content[content.index("check_all()"):]
        for script_name in GENERATOR_SCRIPTS:
            with self.subTest(script=script_name):
                self.assertIn(script_name, check_section)


class DocsLinks_AllResolve(unittest.TestCase):
    """DocsLinks: every internal markdown link target must resolve."""

    catalog: dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = load_json(CATALOG_PATH)

    def test_all_internal_links_resolve(self) -> None:
        errors = validate_docs_links(self.catalog)
        if errors:
            detail = "\n".join(
                f"  {e['source']}:{e['line']}: {e['target']} — {e['error']}"
                for e in errors[:20]
            )
            self.fail(
                f"{len(errors)} broken link(s) found:\n{detail}"
            )

    def test_route_overrides_point_to_existing_files(self) -> None:
        errors = validate_docs_routes(self.catalog)
        if errors:
            detail = "\n".join(
                f"  {e['target']} — {e['error']}" for e in errors
            )
            self.fail(f"{len(errors)} broken route override(s):\n{detail}")


class DocsLinks_CatalogConsistency(unittest.TestCase):
    """DocsLinks: catalog declarations must be internally consistent."""

    catalog: dict[str, Any]

    @classmethod
    def setUpClass(cls) -> None:
        cls.catalog = load_json(CATALOG_PATH)

    def test_root_documents_exist(self) -> None:
        for doc in self.catalog.get("include", {}).get("rootDocuments", []):
            with self.subTest(doc=doc):
                self.assertTrue(
                    (REPO_ROOT / doc).is_file(),
                    f"root document does not exist: {doc}",
                )

    def test_recursive_roots_exist(self) -> None:
        for root in self.catalog.get("include", {}).get("recursiveMarkdownRoots", []):
            with self.subTest(root=root):
                self.assertTrue(
                    (REPO_ROOT / root).is_dir(),
                    f"recursive root is not a directory: {root}",
                )

    def test_classification_rules_reference_valid_sections(self) -> None:
        section_ids = {s["id"] for s in self.catalog.get("sections", [])}
        for rule in self.catalog.get("classificationRules", []):
            with self.subTest(prefix=rule.get("prefix")):
                self.assertIn(
                    rule.get("section"),
                    section_ids,
                    f"classification rule references unknown section",
                )

    def test_featured_paths_exist(self) -> None:
        from validate import FUTURE_ACCEPTANCE_PATHS  # noqa: E402
        import fnmatch

        for path in self.catalog.get("featuredSourcePaths", []):
            with self.subTest(path=path):
                exists = (REPO_ROOT / path).exists()
                is_future = any(
                    fnmatch.fnmatchcase(path, pattern)
                    for pattern in FUTURE_ACCEPTANCE_PATHS
                )
                self.assertTrue(
                    exists or is_future,
                    f"featured path does not exist and is not a known future path: {path}",
                )

    def test_catalog_discovers_documents(self) -> None:
        documents = collect_documents(self.catalog)
        self.assertGreater(
            len(documents), 10,
            "catalog must resolve more than 10 markdown documents",
        )


class DocsLinks_ValidatorScriptExists(unittest.TestCase):
    """DocsLinks: the standalone validator script is wired and importable."""

    def test_validator_script_exists(self) -> None:
        script = REPO_ROOT / "tools" / "site-data" / "validate_docs_links.py"
        self.assertTrue(script.is_file())

    def test_ci_workflow_invokes_link_validation(self) -> None:
        workflow_path = REPO_ROOT / ".github" / "workflows" / "site-data.yml"
        self.assertTrue(workflow_path.is_file(), "site-data CI workflow must exist")
        workflow = workflow_path.read_text(encoding="utf-8")
        self.assertIn("validate_docs_links", workflow)


if __name__ == "__main__":
    unittest.main()
