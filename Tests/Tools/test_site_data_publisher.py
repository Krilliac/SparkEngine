#!/usr/bin/env python3
"""The publication entry point generates and checks docs before replacing output."""

from __future__ import annotations

import argparse
import json
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_docs_health import COMMITTED_AT, EXACT_SHA, LinkFixture, write
import generate
from common import SiteDataError


class OutputReached(RuntimeError):
    """Stop the fixture at the first output mutation after the real input checks."""


class ApiProducerTests(unittest.TestCase):
    def test_architecture_panel_count_has_its_own_generated_metric(self) -> None:
        """Header inventory and registered factory entries describe different facts."""
        metrics = {entry["id"]: entry for entry in generate.collect_metrics(0, generate.module_statistics())}
        self.assertIn("editor.panelHeaders", metrics)
        self.assertIn("editor.panels", metrics)
        self.assertEqual(metrics["editor.panelHeaders"]["unit"], "headers")
        self.assertEqual(metrics["editor.panelHeaders"]["evidence"][0]["path"], "SparkEditor/Source/Panels")

    def test_publication_uses_the_portable_python_api_producer(self) -> None:
        """Publication uses the shell wrapper's real producer on every host."""
        def launch(command, **options):
            self.assertEqual(command, [
                sys.executable, str(generate.REPO_ROOT / "tools" / "docs_contract.py"),
                "generate-api", "--output", str(generate.REPO_ROOT / "docs" / "api"),
            ])
            self.assertEqual(options["environment"]["SPARKENGINE_DOC_SOURCE_SHA"], EXACT_SHA)
            self.assertEqual(options["timeout"], generate.API_GENERATION_TIMEOUT_SECONDS)
            raise OutputReached("producer reached")

        with mock.patch.object(generate, "run_bounded_process", side_effect=launch):
            with self.assertRaises(OutputReached):
                generate.regenerate_api_docs(EXACT_SHA, COMMITTED_AT)


class PublisherDocsTests(unittest.TestCase):
    def publish(self, fixture: LinkFixture, producer=None) -> None:
        source = {"commit": EXACT_SHA, "committedAt": COMMITTED_AT}
        args = argparse.Namespace(
            allow_legacy_contract=False, source_branch=None, source_commit=None,
            committed_at=None, allow_dirty=False, evidence_commit=None,
            exact_evidence_file=None, output=fixture.root / "publication", preserve_existing=True,
        )
        with (
            mock.patch.object(generate, "REPO_ROOT", fixture.root),
            mock.patch.object(generate, "validate_contract", return_value={"docsCatalog": fixture.catalog}),
            mock.patch.object(generate, "repository_source", return_value=source),
            mock.patch.object(generate, "git_dirty_paths", return_value=[]),
            mock.patch.object(generate, "regenerate_api_docs", side_effect=producer),
            mock.patch.object(generate, "ensure_safe_output", side_effect=OutputReached("output reached")),
        ):
            generate.generate(args)

    def test_clean_checkout_generates_api_before_link_check(self) -> None:
        with LinkFixture() as fixture:
            # Simulate the ignored API tree missing in a clean checkout.
            (fixture.api / ".manifest.json").unlink()
            (fixture.api / "README.md").unlink()
            fixture.api.rmdir()
            write(fixture.docs / "Guide.md", "# Guide\n[API](api/README.md#api-home)\n")

            def produce(commit: str, committed_at: str) -> None:
                self.assertEqual((commit, committed_at), (EXACT_SHA, COMMITTED_AT))
                write(fixture.api / "README.md", "# API Home\n")
                fixture.refresh_manifest()

            with self.assertRaises(OutputReached):
                self.publish(fixture, produce)

    def test_broken_link_refuses_to_replace_previous_publication(self) -> None:
        with LinkFixture() as fixture:
            write(fixture.docs / "Guide.md", "# Guide\n[Missing](Missing.md)\n")
            with self.assertRaisesRegex(SiteDataError, "documentation validation failed.*Missing.md"):
                self.publish(fixture)

    def test_broken_anchor_refuses_to_replace_previous_publication(self) -> None:
        with LinkFixture() as fixture:
            write(fixture.docs / "Guide.md", "# Guide\n[API](api/README.md#missing)\n")
            with self.assertRaisesRegex(SiteDataError, "documentation validation failed.*missing"):
                self.publish(fixture)

    def test_route_collision_refuses_to_replace_previous_publication(self) -> None:
        with LinkFixture() as fixture:
            fixture.catalog["routeOverrides"] = {"docs/Guide.md": "api", "docs/api/README.md": "api"}
            with self.assertRaisesRegex(SiteDataError, "documentation validation failed.*collision"):
                self.publish(fixture)

    def test_wrong_commit_api_manifest_refuses_publication(self) -> None:
        with LinkFixture() as fixture:
            manifest = fixture.api / ".manifest.json"
            payload = json.loads(manifest.read_text(encoding="utf-8"))
            payload["sourceCommit"] = "b" * 40
            write(manifest, json.dumps(payload))
            with self.assertRaisesRegex(SiteDataError, "documentation validation failed.*sourceCommit"):
                self.publish(fixture)


if __name__ == "__main__":
    unittest.main()
