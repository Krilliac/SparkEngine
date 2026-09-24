"""Run staging/guard behavior with only the GitHub process boundary simulated."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlparse

from guard_release_mutation import guarded_run
from stage_release_draft import DraftApi, stage
from verify_release_policy import validate_policy


class DraftStagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.inventory = self.root / "expected.txt"
        self.inventory.write_text("a.zip\nb.zip\n")
        for name in ("a.zip", "b.zip"):
            (self.root / name).write_bytes(name.encode())
        self.versioned = True
        self.policy = {"enabled": True, "enforced_by_owner": False}
        self.record = {"id": 7, "tag_name": "v1.2.3", "draft": True, "prerelease": False, "immutable": False}
        self.assets = []
        self.writes = []
        self.metadata_bodies = []
        self.flip_after = None

    def tearDown(self):
        self.temporary.cleanup()

    def run_process(self, command, **kwargs):
        method = command[command.index("--method") + 1] if "--method" in command else "GET"
        endpoint = command[4] if "--method" in command else command[2]
        if method == "GET":
            value = self.assets if "/assets?" in endpoint else self.record
        else:
            self.writes.append((method, endpoint))
            if method == "DELETE":
                asset_id = int(endpoint.rsplit("/", 1)[1])
                self.assets = [asset for asset in self.assets if asset["id"] != asset_id]
                value = None
            elif endpoint.startswith("https://uploads.github.com/"):
                name = parse_qs(urlparse(endpoint).query)["name"][0]
                data = Path(command[command.index("--input") + 1]).read_bytes()
                value = {"id": 10 + len(self.writes), "name": name, "state": "uploaded",
                         "size": len(data), "digest": "sha256:" + hashlib.sha256(data).hexdigest()}
                self.assets.append(value)
            else:
                metadata = json.loads(kwargs["input"])
                self.metadata_bodies.append(metadata)
                self.record.update(metadata)
                value = self.record
            if self.flip_after == len(self.writes):
                self.policy["enabled"] = not self.versioned
        return subprocess.CompletedProcess(command, 0, json.dumps(value).encode() if value is not None else b"", b"")

    def invoke(self, expected_id=0):
        def guarded(command, repository, is_versioned, **kwargs):
            return guarded_run(command, repository, is_versioned, runner=self.run_process,
                               policy_check=lambda repo, channel: validate_policy(
                                   self.policy, channel,
                                   immutable=os.environ.get("RELEASE_IMMUTABLE") == "true"
                                   if "RELEASE_IMMUTABLE" in os.environ else None), **kwargs)
        with patch("stage_release_draft.subprocess.run", side_effect=self.run_process), \
             patch("stage_release_draft.guarded_run", side_effect=guarded):
            return stage(repository="owner/repo", tag=self.record["tag_name"], source_sha="a" * 40,
                         is_versioned=self.versioned, expected_id=expected_id, title="test", body="notes",
                         assets_file=self.inventory, root=self.root, api=DraftApi("owner/repo", self.versioned))

    def test_creates_only_a_draft_and_guards_each_upload(self):
        self.assertEqual(self.invoke(), 7)
        self.assertEqual([method for method, _ in self.writes], ["POST", "POST", "POST"])
        self.assertTrue(self.record["draft"])
        self.assertEqual(self.record["make_latest"], "false")

    def test_policy_flip_after_create_or_upload_blocks_next_write(self):
        for phase in (1, 2):
            self.writes = []
            self.assets = []
            self.policy["enabled"] = True
            self.flip_after = phase
            with self.assertRaisesRegex(ValueError, "immutable"):
                self.invoke()
            self.assertEqual(len(self.writes), phase)
            self.assertTrue(self.record["draft"])

    def test_stable_staging_never_overwrites_an_existing_asset(self):
        self.assets = [{"id": 9, "name": "a.zip", "state": "uploaded", "size": 5, "digest": "sha256:" + "b" * 64}]
        with self.assertRaisesRegex(ValueError, "never be overwritten"):
            self.invoke(expected_id=7)
        self.assertEqual([method for method, _ in self.writes], ["PATCH"])

    def test_existing_draft_update_does_not_retarget_or_change_channel_or_visibility(self):
        self.assertEqual(self.invoke(expected_id=7), 7)
        self.assertEqual(self.metadata_bodies[0], {"name": "test", "body": "notes"})

    def test_nightly_replacement_checks_policy_before_delete_and_upload(self):
        self.versioned = False
        self.policy["enabled"] = True
        self.record.update(tag_name="nightly-123-1-aaaaaaaaaaaa", prerelease=True)
        self.assets = [{"id": 9, "name": "a.zip", "state": "uploaded", "size": 5, "digest": "sha256:" + "b" * 64}]
        old = os.environ.get("RELEASE_IMMUTABLE")
        os.environ["RELEASE_IMMUTABLE"] = "true"
        try:
            self.flip_after = 0  # Immutable policy is checked before any replacement.
            self.assertEqual(self.invoke(expected_id=7), 7)
        finally:
            if old is None:
                os.environ.pop("RELEASE_IMMUTABLE", None)
            else:
                os.environ["RELEASE_IMMUTABLE"] = old
        self.assertEqual([method for method, _ in self.writes], ["PATCH", "DELETE", "POST", "POST"])

    def test_invalid_local_inventory_is_rejected_before_mutation(self):
        self.inventory.write_text("../outside.zip\n")
        with self.assertRaisesRegex(ValueError, "safe flat"):
            self.invoke()
        self.assertEqual(self.writes, [])

    def test_a_public_target_is_never_rewritten_as_a_draft(self):
        self.record["draft"] = False
        with self.assertRaisesRegex(ValueError, "exact mutable draft"):
            self.invoke(expected_id=7)
        self.assertEqual(self.writes, [])


if __name__ == "__main__":
    unittest.main()
