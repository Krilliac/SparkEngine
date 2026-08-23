#!/usr/bin/env python3
"""Regression tests for the GitHub Wiki publication boundary."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "publish-wiki.py"
SPEC = importlib.util.spec_from_file_location("publish_wiki", SCRIPT)
assert SPEC and SPEC.loader
publish_wiki = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publish_wiki)


class PublishWikiTests(unittest.TestCase):
    repository = "Example/SparkEngine"
    revision = "0123456789abcdef0123456789abcdef01234567"

    def test_repository_urls_are_pinned_to_exact_revision(self) -> None:
        authored = (
            "[preset](https://github.com/Example/SparkEngine/blob/Working/CMakePresets.json)\n"
            "![badge](https://raw.githubusercontent.com/Example/SparkEngine/Working/badge.svg)\n"
        )
        source = publish_wiki.WIKI_ROOT / "Build-Guide.md"

        rewritten = publish_wiki.rewrite_markdown(
            authored,
            source,
            repository=self.repository,
            revision=self.revision,
        )

        self.assertIn(f"/blob/{self.revision}/CMakePresets.json", rewritten)
        self.assertIn(f"/{self.revision}/badge.svg", rewritten)
        self.assertNotIn("/Working/", rewritten)

    def test_fenced_examples_are_never_rewritten_or_validated(self) -> None:
        authored = (
            "[real](getting-started/Getting-Started.md)\n"
            "```markdown\n"
            "[example](missing-page.md)\n"
            "<img src=\"../docs/example.png\">\n"
            "```\n"
        )
        source = publish_wiki.WIKI_ROOT / "Home.md"

        rewritten = publish_wiki.rewrite_markdown(
            authored,
            source,
            repository=self.repository,
            revision=self.revision,
        )

        self.assertIn("[real](Getting-Started)", rewritten)
        self.assertIn("[example](missing-page.md)", rewritten)
        self.assertIn('<img src="../docs/example.png">', rewritten)
        self.assertEqual(["Getting-Started"], publish_wiki.internal_targets(rewritten))

    def test_relative_repository_assets_use_exact_revision(self) -> None:
        source = publish_wiki.WIKI_ROOT / "Home.md"
        target = publish_wiki.rewrite_target(
            "../README.md",
            source,
            image=False,
            repository=self.repository,
            revision=self.revision,
        )
        self.assertEqual(
            f"https://github.com/{self.repository}/blob/{self.revision}/README.md",
            target,
        )

    def test_source_pages_reject_symlinked_markdown(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wiki = root / "wiki"
            wiki.mkdir()
            real = root / "outside.md"
            real.write_text("outside\n", encoding="utf-8")
            linked = wiki / "Home.md"
            try:
                linked.symlink_to(real)
            except OSError as error:
                self.skipTest(f"symlinks unavailable: {error}")

            with mock.patch.object(publish_wiki, "REPO_ROOT", root), mock.patch.object(
                publish_wiki, "WIKI_ROOT", wiki
            ):
                with self.assertRaisesRegex(
                    publish_wiki.PublishError, "must not use symlinks"
                ):
                    publish_wiki.source_pages()

    def test_parse_repository_accepts_slug_and_common_remotes(self) -> None:
        self.assertEqual(
            "Krilliac/SparkEngine",
            publish_wiki.parse_repository("Krilliac/SparkEngine"),
        )
        self.assertEqual(
            "Krilliac/SparkEngine",
            publish_wiki.parse_repository(
                "https://github.com/Krilliac/SparkEngine.git"
            ),
        )
        self.assertEqual(
            "Krilliac/SparkEngine",
            publish_wiki.parse_repository("git@github.com:Krilliac/SparkEngine.git"),
        )
        with self.assertRaises(publish_wiki.PublishError):
            publish_wiki.parse_repository("https://example.com/repository.git")


if __name__ == "__main__":
    unittest.main()
