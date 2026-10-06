#!/usr/bin/env python3
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("provision_previous", Path(__file__).with_name("provision-previous-windows-msi.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FakeApi:
    def __init__(self, releases, refs, tags, assets):
        self.repository = "acme/SparkEngine"
        self.releases, self.refs, self.tags, self.assets = releases, refs, tags, assets
        self.release_detail = releases[-1]
        self.on_download = None
        self.calls = []

    def json(self, path):
        self.calls.append(path)
        if "/releases/22" in path:
            return self.release_detail
        if "/releases?" in path:
            page = int(path.rsplit("page=", 1)[1])
            return self.releases if page == 1 else []
        if "/git/ref/tags/" in path:
            return self.refs[path.rsplit("/", 1)[1]]
        if "/git/tags/" in path:
            return self.tags[path.rsplit("/", 1)[1]]
        raise AssertionError(path)

    def download_asset(self, asset_id):
        self.calls.append(f"asset:{asset_id}")
        if self.on_download:
            self.on_download(self, asset_id)
        return self.assets[asset_id]


def asset(name, asset_id, content):
    return {"id": asset_id, "name": name, "size": len(content), "digest": "sha256:" + hashlib.sha256(content).hexdigest()}


class ProvisionTests(unittest.TestCase):
    def fixture(self, version="1.2.3"):
        msi_name = f"SparkEngine-{version}-Windows-AMD64-MinSizeRel-Runtime.msi"
        msi = b"real-msi-fixture"
        commit = "0123456789abcdef0123456789abcdef01234567"
        manifest = json.dumps({
            "schemaVersion": "spark-shipping-package-v1", "commitSHA": commit,
            "profile": "stable-v1", "configuration": "MinSizeRel", "version": version,
            "msi": msi_name, "sha256": hashlib.sha256(msi).hexdigest(),
        }, sort_keys=True).encode() + b"\n"
        release = {"id": 22, "draft": False, "prerelease": False, "immutable": True,
                   "tag_name": "v" + version,
                   "assets": [asset(msi_name, 11, msi), asset("shipping-package-manifest.json", 12, manifest)]}
        api = FakeApi([{"draft": False, "prerelease": True, "tag_name": "v9.0.0"}, release],
                      {"v" + version: {"ref": "refs/tags/v" + version, "object": {"type": "commit", "sha": commit}}}, {},
                      {11: msi, 12: manifest})
        return api, version, commit

    def test_selects_greatest_lower_published_release_and_downloads_by_id(self):
        api, version, commit = self.fixture()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            result = MODULE.provision("acme/SparkEngine", "1.2.4", root / "previous", root / "receipt.json", api=api)
            self.assertEqual(result["previous_version"], version)
            self.assertEqual(result["tag_commit_sha"], commit)
            self.assertEqual(result["release_id"], 22)
            self.assertIs(result["release_immutable"], True)
            self.assertEqual([call for call in api.calls if call.startswith("asset:")], ["asset:11", "asset:12"])
            self.assertTrue((root / "previous/packages" / result["msi"]["name"]).is_file())
            self.assertEqual(json.loads((root / "receipt.json").read_text())["schema"], "spark-previous-windows-msi-v1")
            self.assertNotIn("Authorization", (root / "receipt.json").read_text())

    def test_resolves_annotated_tag_to_commit(self):
        api, _, commit = self.fixture()
        api.refs["v1.2.3"]["object"] = {"type": "tag", "sha": "abcdefabcdefabcdefabcdefabcdefabcdefabcd"}
        api.tags["abcdefabcdefabcdefabcdefabcdefabcdefabcd"] = {"object": {"type": "commit", "sha": commit}}
        with tempfile.TemporaryDirectory() as temp:
            result = MODULE.provision("acme/SparkEngine", "1.2.4", Path(temp) / "out", Path(temp) / "receipt", api=api)
            self.assertEqual(result["tag_commit_sha"], commit)

    def test_no_predecessor_fails_and_creates_no_output(self):
        api, _, _ = self.fixture()
        api.releases = [{"draft": False, "prerelease": False, "tag_name": "v1.2.4"}]
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "out"
            with self.assertRaises(MODULE.ProvisionError):
                MODULE.provision("acme/SparkEngine", "1.2.4", output, Path(temp) / "receipt", api=api)
            self.assertFalse(output.exists())

    def test_v1_requires_exact_v090_predecessor(self):
        api, _, _ = self.fixture()
        api.releases.insert(0, {"id": 21, "draft": False, "prerelease": False,
                                "immutable": True, "tag_name": "v0.8.0"})
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(MODULE.ProvisionError, "v0.9.0"):
                MODULE.provision("acme/SparkEngine", "1.0.0", Path(temp) / "out", Path(temp) / "receipt", api=api)

    def test_v1_accepts_exact_immutable_v090_predecessor(self):
        api, _, commit = self.fixture("0.9.0")
        with tempfile.TemporaryDirectory() as temp:
            result = MODULE.provision("acme/SparkEngine", "1.0.0", Path(temp) / "out", Path(temp) / "receipt", api=api)
            self.assertEqual(result["previous_version"], "0.9.0")
            self.assertEqual(result["tag_commit_sha"], commit)

    def test_mutable_predecessor_is_rejected(self):
        api, _, _ = self.fixture()
        api.releases[-1]["immutable"] = False
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(MODULE.ProvisionError, "immutable"):
                MODULE.provision("acme/SparkEngine", "1.2.4", Path(temp) / "out", Path(temp) / "receipt", api=api)

    def test_missing_immutable_field_is_rejected(self):
        api, _, _ = self.fixture()
        del api.releases[-1]["immutable"]
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(MODULE.ProvisionError, "immutable"):
                MODULE.provision("acme/SparkEngine", "1.2.4", Path(temp) / "out", Path(temp) / "receipt", api=api)

    def test_predecessor_immutability_drift_is_rejected(self):
        api, _, _ = self.fixture()
        def drift(fake, asset_id):
            if asset_id == 12:
                fake.release_detail["immutable"] = False
        api.on_download = drift
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(MODULE.ProvisionError, "immutable"):
                MODULE.provision("acme/SparkEngine", "1.2.4", Path(temp) / "out", Path(temp) / "receipt", api=api)

    def test_digest_mismatch_removes_partial_output(self):
        api, _, _ = self.fixture()
        api.assets[11] = b"tampered"
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "out"
            with self.assertRaises(MODULE.ProvisionError):
                MODULE.provision("acme/SparkEngine", "1.2.4", output, Path(temp) / "receipt", api=api)
            self.assertFalse(output.exists())

    def test_existing_output_is_rejected(self):
        api, _, _ = self.fixture()
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "out"
            output.mkdir()
            with self.assertRaises(MODULE.ProvisionError):
                MODULE.provision("acme/SparkEngine", "1.2.4", output, Path(temp) / "receipt", api=api)

    def test_dangling_receipt_symlink_is_rejected_without_following(self):
        api, _, _ = self.fixture()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receipt = root / "receipt.json"
            try:
                os.symlink(root / "missing-target.json", receipt)
            except (OSError, NotImplementedError):
                self.skipTest("symbolic links are unavailable on this platform")
            with self.assertRaises(MODULE.ProvisionError):
                MODULE.provision("acme/SparkEngine", "1.2.4", root / "out", receipt, api=api)

    def test_release_identity_drift_after_download_is_rejected(self):
        api, _, _ = self.fixture()
        changed = {"draft": False, "prerelease": False, "tag_name": "v1.2.2"}
        def drift(fake, asset_id):
            if asset_id == 12:
                fake.release_detail["tag_name"] = changed["tag_name"]
        api.on_download = drift
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(MODULE.ProvisionError, "release identity changed"):
                MODULE.provision("acme/SparkEngine", "1.2.4", Path(temp) / "out", Path(temp) / "receipt", api=api)

    def test_release_asset_metadata_drift_after_download_is_rejected(self):
        api, _, _ = self.fixture()
        def drift(fake, asset_id):
            if asset_id == 12:
                fake.release_detail["assets"][1]["id"] = 99
        api.on_download = drift
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(MODULE.ProvisionError, "asset metadata changed"):
                MODULE.provision("acme/SparkEngine", "1.2.4", Path(temp) / "out", Path(temp) / "receipt", api=api)

    def test_tag_target_drift_after_download_is_rejected(self):
        api, _, _ = self.fixture()
        def drift(fake, asset_id):
            if asset_id == 12:
                fake.refs["v1.2.3"]["object"]["sha"] = "fedcba9876543210fedcba9876543210fedcba98"
        api.on_download = drift
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(MODULE.ProvisionError, "tag target changed"):
                MODULE.provision("acme/SparkEngine", "1.2.4", Path(temp) / "out", Path(temp) / "receipt", api=api)


if __name__ == "__main__":
    unittest.main()
