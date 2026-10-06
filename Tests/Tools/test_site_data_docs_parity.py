#!/usr/bin/env python3
"""DOC-400 live docs parity: routes, search records and source links of a real bundle agree.

generate.py runs once into a temporary directory, the same way
tools/site-data/runtime/test/verifyBundle.test.mjs does. The untouched
publication must have no parity errors. Each mutation below is published into
one copy of it with every pointer hash recomputed, so only the parity check
(tools/site-data/docs_parity.py, called by validate_published_bundle) can
reject it.

This lives outside test_site_data_contract.py because the site-data workflow
runs that suite under a 5-minute budget, and a full generate does not fit.
"""

from __future__ import annotations

import copy
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any, Callable
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

from common import REPOSITORY_URL, SiteDataError, canonical_json_bytes  # noqa: E402
from docs_parity import published_docs_parity_errors  # noqa: E402
import generate as site_data_generate  # noqa: E402
from validate import validate_published_bundle  # noqa: E402

README_SLUG = "project/readme"


def load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def write(path: Path, payload: bytes) -> dict[str, Any]:
    path.write_bytes(payload)
    return {"sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)}


class LiveDocsParityTests(unittest.TestCase):
    root: Path
    scratch: Path
    variant: Path
    originals: dict[Path, bytes]

    @classmethod
    def setUpClass(cls) -> None:
        cls.scratch = Path(tempfile.mkdtemp(prefix="spark-docs-parity-"))
        cls.root = cls.scratch / "site-data"
        result = subprocess.run(
            [sys.executable, str(REPO_ROOT / "tools" / "site-data" / "generate.py"),
             "--allow-dirty", "--skip-doc-health", "--output", str(cls.root)],
            cwd=REPO_ROOT, capture_output=True, text=True, timeout=540, check=False,
        )
        if result.returncode != 0:
            shutil.rmtree(cls.scratch, ignore_errors=True)
            raise AssertionError(f"generate.py failed:\n{result.stdout}\n{result.stderr}")
        # Mutations go to one full copy (the reader refuses hard links); the
        # files they rewrite are restored before each one.
        cls.variant = Path(shutil.copytree(cls.root, cls.scratch / "variant"))
        latest = load(cls.variant / "latest.json")
        cls.originals = {
            path: path.read_bytes()
            for path in (
                cls.variant / "latest.json",
                cls.variant / latest["files"]["bundle"]["path"],
                cls.variant / latest["files"]["docsSearch"]["path"],
            )
        }

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls.scratch, ignore_errors=True)

    def latest_and_bundle(self, root: Path) -> tuple[dict[str, Any], dict[str, Any]]:
        latest = load(root / "latest.json")
        return latest, load(root / latest["files"]["bundle"]["path"])

    def publish_variant(
        self,
        mutate_bundle: Callable[[dict[str, Any]], None] | None = None,
        mutate_search: Callable[[dict[str, Any]], None] | None = None,
        page_content: Callable[[str], str] | None = None,
    ) -> Path:
        """The generated publication with one mutation and consistent pointers."""
        for path, payload in self.originals.items():
            path.write_bytes(payload)
        variant = self.variant
        latest, bundle = self.latest_and_bundle(variant)
        docs = bundle["docs"]
        if page_content is not None:
            document = next(item for item in docs["documents"] if item["slug"] == README_SLUG)
            page = load(variant / document["published"]["path"])
            page["content"] = page_content(page["content"])
            payload = canonical_json_bytes(page)
            digest = hashlib.sha256(payload).hexdigest()
            relative = f"{Path(document['published']['path']).parent.as_posix()}/{digest}.json"
            write(variant / relative, payload)
            published = {"path": relative, "sha256": digest, "bytes": len(payload)}
            document.update(published=published, contentPath=relative, contentSha256=digest,
                            contentBytes=len(payload))
            docs["filesBySlug"][README_SLUG] = published
        if mutate_search is not None:
            search_path = variant / latest["files"]["docsSearch"]["path"]
            search = load(search_path)
            mutate_search(search)
            info = write(search_path, canonical_json_bytes(search))
            latest["files"]["docsSearch"].update(info)
            docs["searchSha256"], docs["searchBytes"] = info["sha256"], info["bytes"]
        if mutate_bundle is not None:
            mutate_bundle(bundle)
        info = write(variant / latest["files"]["bundle"]["path"], canonical_json_bytes(bundle))
        latest["files"]["bundle"].update(info)
        write(variant / "latest.json", canonical_json_bytes(latest))
        return variant

    def test_generated_publication_has_no_parity_errors(self) -> None:
        latest, bundle = self.latest_and_bundle(self.root)
        self.assertGreater(len(bundle["docs"]["documents"]), 100)
        self.assertEqual(published_docs_parity_errors(self.root, bundle, latest), [])
        validate_published_bundle(self.root)
        # The badge fix: a relative target behind a nested image link is rewritten.
        readme = next(item for item in bundle["docs"]["documents"] if item["slug"] == README_SLUG)
        content = load(self.root / readme["published"]["path"])["content"]
        commit = latest["source"]["commit"]
        self.assertIn(f")]({REPOSITORY_URL}/tree/{commit}/Tests)", content)
        self.assertNotIn(")](Tests)", content)

    def test_generated_api_documents_pass_without_a_generated_tree(self) -> None:
        """The publish job validates from a fresh checkout, where gitignored docs/api does not exist."""
        latest, bundle = self.latest_and_bundle(self.root)
        api_documents = [item for item in bundle["docs"]["documents"] if item["sourcePath"].startswith("docs/api/")]
        self.assertGreater(len(api_documents), 0)
        on_disk = site_data_generate.collect_document_sources

        def fresh_checkout(catalog: dict[str, Any]) -> list[Path]:
            api_root = REPO_ROOT / "docs" / "api"
            return [path for path in on_disk(catalog) if api_root not in path.parents]

        with mock.patch.object(site_data_generate, "collect_document_sources", fresh_checkout):
            self.assertEqual(published_docs_parity_errors(self.root, bundle, latest), [])
            validate_published_bundle(self.root)

            # Only Markdown paths the catalog selects are accepted there.
            commit = latest["source"]["commit"]
            for bad_path in ("docs/api/Not-Markdown.txt", "docs/api/../../README.md"):
                with self.subTest(source_path=bad_path):
                    def api_source(bundle_value: dict[str, Any], path: str = bad_path) -> None:
                        document = next(
                            item for item in bundle_value["docs"]["documents"]
                            if item["slug"] == api_documents[0]["slug"]
                        )
                        document["sourcePath"] = path
                        document["sourceUrl"] = f"{REPOSITORY_URL}/blob/{commit}/{path}"

                    def api_record(search: dict[str, Any], path: str = bad_path) -> None:
                        next(
                            record for record in search["records"] if record["slug"] == api_documents[0]["slug"]
                        )["sourcePath"] = path

                    variant = self.publish_variant(mutate_bundle=api_source, mutate_search=api_record)
                    with self.assertRaises(SiteDataError) as raised:
                        validate_published_bundle(variant)
                    self.assertIn("is not a published document source", str(raised.exception))

    def test_each_mutation_is_rejected_by_the_published_bundle_validator(self) -> None:
        latest, bundle = self.latest_and_bundle(self.root)
        commit = latest["source"]["commit"]
        slugs = [item["slug"] for item in bundle["docs"]["documents"]]
        other = next(slug for slug in slugs if slug != README_SLUG)

        def doc(target: dict[str, Any], slug: str) -> dict[str, Any]:
            return next(item for item in target["docs"]["documents"] if item["slug"] == slug)

        def extra_record(search: dict[str, Any]) -> None:
            record = copy.deepcopy(search["records"][0])
            record["slug"] = "nowhere/missing"
            search["records"].append(record)

        def drop_record(search: dict[str, Any]) -> None:
            search["records"] = [record for record in search["records"] if record["slug"] != other]

        def record_source(search: dict[str, Any]) -> None:
            next(record for record in search["records"] if record["slug"] == other)["sourcePath"] = "README.md"

        def document_source(bundle_value: dict[str, Any]) -> None:
            document = doc(bundle_value, README_SLUG)
            document["sourcePath"] = "wiki/Does-Not-Exist.md"
            document["sourceUrl"] = f"{REPOSITORY_URL}/blob/{commit}/wiki/Does-Not-Exist.md"

        def search_follows_document_source(search: dict[str, Any]) -> None:
            next(record for record in search["records"] if record["slug"] == README_SLUG)["sourcePath"] = (
                "wiki/Does-Not-Exist.md"
            )

        def source_url_commit(bundle_value: dict[str, Any]) -> None:
            doc(bundle_value, README_SLUG)["sourceUrl"] = f"{REPOSITORY_URL}/blob/{'1' * 40}/README.md"

        cases: dict[str, tuple[dict[str, Any], str]] = {
            "search-record-for-missing-slug": (
                {"mutate_search": extra_record}, "search record 'nowhere/missing' has no document"),
            "document-missing-from-search": (
                {"mutate_search": drop_record}, f"document {other!r} is missing from search"),
            "search-source-path-changed": (
                {"mutate_search": record_source}, f"search sourcePath differs for {other!r}"),
            "document-source-path-changed": (
                {"mutate_bundle": document_source, "mutate_search": search_follows_document_source},
                "is not a published document source"),
            "source-url-other-commit": (
                {"mutate_bundle": source_url_commit}, "sourceUrl is not pinned"),
            "relative-link-left-unresolved": (
                {"page_content": lambda text: text + "\n[x](../nowhere.md)\n"}, "'../nowhere.md': unresolved route"),
            "unknown-docs-slug": (
                {"page_content": lambda text: text + "\n[x](/docs/nowhere/at-all)\n"}, "unknown docs slug"),
            "missing-anchor": (
                {"page_content": lambda text: text + f"\n[x](/docs/{other}#no-such-heading-here)\n"},
                "anchor #no-such-heading-here is not a heading"),
            "same-page-missing-anchor": (
                {"page_content": lambda text: text + "\n[x](#no-such-heading-here)\n"},
                "same-page anchor #no-such-heading-here is not a heading"),
            "repository-link-other-commit": (
                {"page_content": lambda text: text + f"\n[x]({REPOSITORY_URL}/blob/Working/README.md)\n"},
                "repository link is not pinned to the source commit"),
        }
        for name, (mutation, fragment) in cases.items():
            with self.subTest(mutation=name):
                variant = self.publish_variant(**mutation)
                with self.assertRaises(SiteDataError) as raised:
                    validate_published_bundle(variant)
                message = str(raised.exception)
                self.assertIn(fragment, message)
                # Pointers were republished consistently, so parity is the only failure.
                self.assertIn("published bundle validation failed with 1 error(s)", message)


class SplitDocumentAnchorTests(unittest.TestCase):
    """A same-page anchor into another section of a split document follows the heading to its page."""

    def test_cross_section_anchors_are_retargeted_to_the_owning_page(self) -> None:
        filler = "Filler text for the size threshold.\n" * (site_data_generate.LARGE_DOCUMENT_BYTES // 36 + 1)
        content = (
            "# Big\n\nIntro [to b](#b-section) and [deep](#deep-heading).\n\n"
            "## A section\n\nSee [deep](#deep-heading), [local](#a-local) and [b](#b-section).\n\n"
            "### A local\n\n" + filler +
            "## B section\n\n### Deep heading\n\nBack to [a](#a-local).\n"
        )
        document = {"slug": "big", "title": "Big", "content": content, "sourcePath": "wiki/Big.md"}
        parent, first, second = site_data_generate.split_large_document(document)
        self.assertEqual([first["slug"], second["slug"]], ["big/a-section", "big/b-section"])
        self.assertIn("[to b](/docs/big/b-section)", parent["content"])
        self.assertIn("[deep](/docs/big/b-section#deep-heading)", parent["content"])
        self.assertIn("[deep](/docs/big/b-section#deep-heading)", first["content"])
        self.assertIn("[local](#a-local)", first["content"])
        self.assertIn("[b](/docs/big/b-section)", first["content"])
        self.assertIn("[a](/docs/big/a-section#a-local)", second["content"])


if __name__ == "__main__":
    unittest.main()
