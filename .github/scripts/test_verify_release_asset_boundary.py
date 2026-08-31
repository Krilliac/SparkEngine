#!/usr/bin/env python3
"""Acceptance tests for the exact GitHub release asset boundary."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from verify_release_asset_boundary import BoundaryError, verify_release_asset_boundary


class ReleaseAssetBoundaryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.names = ["SparkEngine.zip", "SparkEngine-Exact-CI-Evidence.json"]
        for name, payload in zip(self.names, (b"engine\n", b'{"schemaVersion":2}\n')):
            (self.root / name).write_bytes(payload)
        self.digests = {
            name: "sha256:" + hashlib.sha256((self.root / name).read_bytes()).hexdigest()
            for name in self.names
        }
        self.expected_names = self.root / "expected-release-assets.txt"
        self.expected_names.write_text("\n".join(self.names), encoding="utf-8")
        self.expected_digests = self.root / "expected-release-digests.txt"
        self.expected_digests.write_text(
            "\n".join(f"{self.digests[name][7:]}  {name}" for name in self.names),
            encoding="utf-8",
        )
        self.release_path = self.root / "release.json"
        self.assets_path = self.root / "assets.json"
        self.ledger_path = self.root / "ledger.json"
        self.release = {
            "id": 700,
            "tag_name": "stable-v1",
            "draft": True,
            "prerelease": False,
            "immutable": False,
        }
        self.assets = [
            {
                "id": 801 + index,
                "name": name,
                "state": "uploaded",
                "size": (self.root / name).stat().st_size,
                "digest": self.digests[name],
                "download_count": 0,
                "uploader": {"id": 41898282, "login": "github-actions[bot]"},
            }
            for index, name in enumerate(self.names)
        ]
        self.ledger = {
            "schemaVersion": 2,
            "pending": {
                "phase": "staged",
                "targetReleaseId": 700,
                "targetTag": "stable-v1",
                "isVersioned": True,
                "targetDraftAtPrepare": True,
                "preparedAssetIds": [asset["id"] for asset in self.assets],
                "evidenceReplacement": None,
                "draftCleanup": [],
            },
            "assets": [
                {
                    "id": asset["id"],
                    "releaseId": 700,
                    "name": asset["name"],
                    "downloadCount": 0,
                    "digest": asset["digest"],
                    "state": "uploaded",
                }
                for asset in self.assets
            ],
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _write_inputs(self, *, pages=None) -> None:
        self.release_path.write_text(json.dumps(self.release), encoding="utf-8")
        self.assets_path.write_text(
            json.dumps([self.assets] if pages is None else pages), encoding="utf-8"
        )
        self.ledger_path.write_text(json.dumps(self.ledger), encoding="utf-8")

    def _invoke(
        self,
        *,
        expected_draft: bool = True,
        release_tag: str = "stable-v1",
        is_versioned: bool = True,
    ) -> None:
        verify_release_asset_boundary(
            release_json=self.release_path,
            assets_json=self.assets_path,
            ledger_json=self.ledger_path,
            expected_assets_file=self.expected_names,
            expected_digests_file=self.expected_digests,
            asset_directory=self.root,
            release_id=700,
            release_tag=release_tag,
            is_versioned=is_versioned,
            expected_draft=expected_draft,
        )

    def _verify(self, *, expected_draft: bool = True) -> None:
        self._write_inputs()
        self._invoke(expected_draft=expected_draft)

    def test_accepts_exact_draft_and_public_boundary(self) -> None:
        self._verify()
        self.release["draft"] = False
        self.assets[0]["download_count"] = 3
        self._verify(expected_draft=False)

    def test_accepts_new_release_staged_after_absent_prepare_target(self) -> None:
        self.ledger["pending"]["targetDraftAtPrepare"] = None
        self._verify()

    def test_accepts_staged_nightly_that_was_public_before_the_pre_hide_checkpoint(self) -> None:
        self.release.update({"tag_name": "nightly", "prerelease": True})
        self.ledger["pending"].update(
            {
                "targetTag": "nightly",
                "isVersioned": False,
                "targetDraftAtPrepare": False,
            }
        )
        self._write_inputs()
        self._invoke(release_tag="nightly", is_versioned=False)

    def test_rejects_release_identity_visibility_channel_or_immutability_drift(self) -> None:
        mutations = (
            ("id", 701),
            ("tag_name", "moved"),
            ("draft", False),
            ("prerelease", True),
            ("immutable", True),
        )
        for key, value in mutations:
            with self.subTest(key=key):
                original = self.release[key]
                self.release[key] = value
                with self.assertRaises(BoundaryError):
                    self._verify()
                self.release[key] = original

    def test_rejects_missing_extra_or_duplicate_release_assets(self) -> None:
        original = copy.deepcopy(self.assets)
        for mutation in ("missing", "extra", "duplicate-id", "duplicate-name"):
            with self.subTest(mutation=mutation):
                self.assets = copy.deepcopy(original)
                if mutation == "missing":
                    self.assets.pop()
                elif mutation == "extra":
                    self.assets.append(copy.deepcopy(self.assets[0]))
                    self.assets[-1].update({"id": 999, "name": "unexpected.zip"})
                elif mutation == "duplicate-id":
                    self.assets[1]["id"] = self.assets[0]["id"]
                else:
                    self.assets[1]["name"] = self.assets[0]["name"]
                with self.assertRaises(BoundaryError):
                    self._verify()
        self.assets = original

    def test_rejects_any_asset_size_digest_state_or_uploader_drift(self) -> None:
        asset = self.assets[0]
        mutations = (
            ("size", asset["size"] + 1),
            ("digest", "sha256:" + "0" * 64),
            ("state", "starter"),
        )
        for key, value in mutations:
            with self.subTest(key=key):
                original = asset[key]
                asset[key] = value
                with self.assertRaises(BoundaryError):
                    self._verify()
                asset[key] = original
        for key, value in (("id", 1), ("login", "octocat")):
            with self.subTest(uploader=key):
                original = asset["uploader"][key]
                asset["uploader"][key] = value
                with self.assertRaises(BoundaryError):
                    self._verify()
                asset["uploader"][key] = original

    def test_rejects_reuploaded_asset_or_ledger_binding_drift(self) -> None:
        mutations = (
            ("preparedAssetIds", [801, 999]),
            ("targetReleaseId", 701),
            ("targetTag", "moved"),
            ("targetDraftAtPrepare", False),
        )
        pending = self.ledger["pending"]
        for key, value in mutations:
            with self.subTest(key=key):
                original = pending[key]
                pending[key] = value
                with self.assertRaises(BoundaryError):
                    self._verify()
                pending[key] = original
        entry = self.ledger["assets"][0]
        for key, value in (
            ("releaseId", 701),
            ("name", "moved.zip"),
            ("digest", "sha256:" + "0" * 64),
            ("state", "starter"),
        ):
            with self.subTest(ledger=key):
                original = entry[key]
                entry[key] = value
                with self.assertRaises(BoundaryError):
                    self._verify()
                entry[key] = original

    def test_rejects_local_byte_or_type_drift_and_download_regression(self) -> None:
        path = self.root / self.names[0]
        original = path.read_bytes()
        path.write_bytes(original + b"x")
        with self.assertRaises(BoundaryError):
            self._verify()
        path.write_bytes(original)

        self.ledger["schemaVersion"] = 2.0
        with self.assertRaises(BoundaryError):
            self._verify()
        self.ledger["schemaVersion"] = 2

        self.release["draft"] = False
        self.assets[0]["download_count"] = 2
        self.ledger["assets"][0]["downloadCount"] = 3
        with self.assertRaises(BoundaryError):
            self._verify(expected_draft=False)

    def test_rejects_duplicate_critical_json_keys(self) -> None:
        self._write_inputs()
        release_text = self.release_path.read_text(encoding="utf-8")
        self.release_path.write_text(
            release_text.replace('"id": 700', '"id": 701, "id": 700', 1),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(BoundaryError, "repeats key 'id'"):
            self._invoke()

        self._write_inputs()
        assets_text = self.assets_path.read_text(encoding="utf-8")
        assets_text = assets_text.replace(
            f'"digest": "{self.assets[0]["digest"]}"',
            f'"digest": "sha256:{"0" * 64}", "digest": "{self.assets[0]["digest"]}"',
            1,
        )
        self.assets_path.write_text(assets_text, encoding="utf-8")
        with self.assertRaisesRegex(BoundaryError, "repeats key 'digest'"):
            self._invoke()

        self._write_inputs()
        assets_text = self.assets_path.read_text(encoding="utf-8")
        self.assets_path.write_text(
            assets_text.replace('"id": 41898282', '"id": 1, "id": 41898282', 1),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(BoundaryError, "repeats key 'id'"):
            self._invoke()

    def test_rejects_nonprogressing_or_post_terminal_asset_pages(self) -> None:
        for pages in ([[], self.assets], [[self.assets[0]], [self.assets[1]]]):
            with self.subTest(page_sizes=[len(page) for page in pages]):
                self._write_inputs(pages=pages)
                with self.assertRaises(BoundaryError):
                    self._invoke()


if __name__ == "__main__":
    unittest.main(verbosity=2)
