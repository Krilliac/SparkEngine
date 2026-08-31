#!/usr/bin/env python3
"""Tests for ID-bound release publication recovery."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

from recover_release_publication import (
    DurableAssetSnapshot,
    RecoveryError,
    main,
    recover_durable_publication,
    recover_release_publication,
    resolve_durable_recovery_target,
)


REPO_ROOT = Path(__file__).resolve().parents[2]


def release(**changes):
    value = {
        "id": 700,
        "tag_name": "stable-v1",
        "draft": False,
        "prerelease": False,
        "immutable": False,
    }
    value.update(changes)
    return value


class FakeApi:
    def __init__(
        self,
        value,
        *,
        assets=(),
        hide_failures=0,
        publish_failures=0,
        publish_then_failures=0,
        refuse_to_hide=False,
        refuse_to_publish=False,
    ):
        self.value = copy.deepcopy(value)
        self.assets = copy.deepcopy(list(assets))
        self.hide_failures = hide_failures
        self.publish_failures = publish_failures
        self.publish_then_failures = publish_then_failures
        self.refuse_to_hide = refuse_to_hide
        self.refuse_to_publish = refuse_to_publish
        self.hide_calls = 0
        self.publish_calls = 0
        self.get_calls = 0
        self.asset_get_calls = 0

    def get_release(self, release_id):
        self.get_calls += 1
        return copy.deepcopy(self.value)

    def hide_release(self, release_id):
        self.hide_calls += 1
        if self.hide_failures:
            self.hide_failures -= 1
            raise RuntimeError("transient PATCH failure")
        if not self.refuse_to_hide:
            self.value["draft"] = True

    def get_release_assets(self, release_id):
        self.asset_get_calls += 1
        return copy.deepcopy(self.assets)

    def publish_release(self, release_id, *, prerelease):
        self.publish_calls += 1
        if self.publish_failures:
            self.publish_failures -= 1
            raise RuntimeError("transient PATCH failure")
        if not self.refuse_to_publish:
            self.value["draft"] = False
            self.value["prerelease"] = prerelease
        if self.publish_then_failures:
            self.publish_then_failures -= 1
            raise RuntimeError("PATCH response was lost after publication")


class RecoverReleasePublicationTests(unittest.TestCase):
    def _recover(self, api):
        return recover_release_publication(
            api,
            release_id=700,
            release_tag="stable-v1",
            expected_prerelease=False,
            attempts=3,
            sleep_seconds=0,
        )

    def test_already_draft_is_a_verified_noop(self):
        api = FakeApi(release(draft=True))
        result = self._recover(api)
        self.assertFalse(result.mutated)
        self.assertEqual(api.hide_calls, 0)
        self.assertEqual(api.get_calls, 1)

    def test_public_release_is_hidden_and_verified_with_a_fresh_get(self):
        api = FakeApi(release())
        result = self._recover(api)
        self.assertTrue(result.mutated)
        self.assertEqual(api.hide_calls, 1)
        self.assertGreaterEqual(api.get_calls, 2)
        self.assertTrue(api.value["draft"])

    def test_transient_patch_failure_is_retried(self):
        api = FakeApi(release(), hide_failures=1)
        result = self._recover(api)
        self.assertTrue(result.mutated)
        self.assertEqual(api.hide_calls, 2)
        self.assertGreaterEqual(api.get_calls, 3)

    def test_tag_or_channel_drift_is_hidden_without_overwriting_that_drift(self):
        api = FakeApi(release(tag_name="moved", prerelease=True))
        result = self._recover(api)
        self.assertTrue(result.mutated)
        self.assertFalse(result.tag_matches)
        self.assertFalse(result.channel_matches)
        self.assertEqual(api.value["tag_name"], "moved")
        self.assertTrue(api.value["prerelease"])

    def test_wrong_id_or_immutable_release_is_never_mutated(self):
        for value in (release(id=701), release(immutable=True)):
            with self.subTest(value=value):
                api = FakeApi(value)
                with self.assertRaises(RecoveryError):
                    self._recover(api)
                self.assertEqual(api.hide_calls, 0)

    def test_unverified_redraft_fails_after_bounded_attempts(self):
        api = FakeApi(release(), refuse_to_hide=True)
        with self.assertRaisesRegex(RecoveryError, "fresh release GET"):
            self._recover(api)
        self.assertEqual(api.hide_calls, 3)
        self.assertEqual(api.get_calls, 6)


class DurableRecoveryTargetTests(unittest.TestCase):
    def pending(self, **changes):
        value = {
            "operation_id": "900:2",
            "target_tag": "v2.3.4",
            "is_versioned": True,
            "source_sha": "a" * 40,
            "phase": "staged",
            "target_release_id": 700,
            "target_draft_at_prepare": None,
            "prepared_asset_ids": frozenset({41}),
        }
        value.update(changes)
        return SimpleNamespace(**value)

    def test_exact_failed_run_resolves_only_its_staged_release(self):
        target = resolve_durable_recovery_target(
            self.pending(), run_id=900, run_attempt=2, source_sha="a" * 40
        )
        self.assertEqual(target.release_id, 700)
        self.assertEqual(target.release_tag, "v2.3.4")
        self.assertFalse(target.expected_prerelease)

    def test_other_run_or_unstaged_operation_is_a_safe_noop(self):
        for pending in (
            None,
            self.pending(operation_id="901:2"),
            self.pending(source_sha="b" * 40),
            self.pending(phase="prepared", target_draft_at_prepare=True),
        ):
            with self.subTest(pending=pending):
                self.assertIsNone(
                    resolve_durable_recovery_target(
                        pending, run_id=900, run_attempt=2, source_sha="a" * 40
                    )
                )

    def test_preexisting_public_release_is_not_a_recovery_target(self):
        self.assertIsNone(
            resolve_durable_recovery_target(
                self.pending(target_draft_at_prepare=False),
                run_id=900,
                run_attempt=2,
                source_sha="a" * 40,
            )
        )

    def test_prepared_public_nightly_resolves_exact_asset_bound_restore(self):
        assets = {
            41: SimpleNamespace(
                asset_id=41,
                release_id=700,
                name="SparkEngine.zip",
                download_count=12,
                digest="sha256:" + "1" * 64,
                state="uploaded",
            )
        }
        target = resolve_durable_recovery_target(
            self.pending(
                phase="prepared",
                target_tag="nightly",
                is_versioned=False,
                target_draft_at_prepare=False,
            ),
            assets=assets,
            run_id=900,
            run_attempt=2,
            source_sha="a" * 40,
        )
        self.assertEqual(target.action, "restore-public-nightly")
        self.assertEqual(
            target.expected_assets,
            (
                DurableAssetSnapshot(
                    41,
                    "SparkEngine.zip",
                    12,
                    "sha256:" + "1" * 64,
                    "uploaded",
                ),
            ),
        )

    def test_staged_nightly_from_public_snapshot_is_redrafted_not_restored(self):
        target = resolve_durable_recovery_target(
            self.pending(
                target_tag="nightly",
                is_versioned=False,
                target_draft_at_prepare=False,
            ),
            run_id=900,
            run_attempt=2,
            source_sha="a" * 40,
        )
        self.assertEqual(target.action, "redraft")

    def test_matching_malformed_target_fails_closed(self):
        for pending in (
            self.pending(target_release_id=None),
            self.pending(target_tag="nightly"),
            self.pending(is_versioned=False, target_tag="v2.3.4"),
        ):
            with self.subTest(pending=pending):
                with self.assertRaises(RecoveryError):
                    resolve_durable_recovery_target(
                        pending, run_id=900, run_attempt=2, source_sha="a" * 40
                    )

    def test_state_file_mode_is_a_noop_without_a_matching_pending_operation(self):
        state = {
            "schemaVersion": 2,
            "initialized": True,
            "baseTotal": 0,
            "baseInstallerTotal": 0,
            "total": 0,
            "installerTotal": 0,
            "assets": [],
            "liveAssetIds": [],
            "pending": None,
            "updated": "2026-08-31T00:00:00Z",
        }
        with tempfile.TemporaryDirectory() as raw:
            path = Path(raw) / "downloads-data.json"
            path.write_text(json.dumps(state), encoding="utf-8")
            status = main(
                [
                    "--repository", "Krilliac/SparkEngine",
                    "--state-file", str(path),
                    "--run-id", "900",
                    "--run-attempt", "2",
                    "--source-sha", "a" * 40,
                ]
            )
        self.assertEqual(status, 0)


class PreparedNightlyVisibilityRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.expected = (
            DurableAssetSnapshot(
                41,
                "SparkEngine.zip",
                12,
                "sha256:" + "1" * 64,
                "uploaded",
            ),
        )
        self.live_assets = [
            {
                "id": 41,
                "name": "SparkEngine.zip",
                "download_count": 12,
                "digest": "sha256:" + "1" * 64,
                "state": "uploaded",
            }
        ]

    def recover(self, api):
        return recover_durable_publication(
            api,
            release_id=700,
            release_tag="nightly",
            expected_prerelease=True,
            action="restore-public-nightly",
            expected_assets=self.expected,
            attempts=3,
            sleep_seconds=0,
        )

    def test_hidden_unchanged_nightly_is_restored_and_freshly_verified(self):
        api = FakeApi(
            release(tag_name="nightly", draft=True, prerelease=True),
            assets=self.live_assets,
        )
        result = self.recover(api)
        self.assertTrue(result.mutated)
        self.assertFalse(api.value["draft"])
        self.assertEqual(api.publish_calls, 1)
        self.assertGreaterEqual(api.get_calls, 2)
        self.assertGreaterEqual(api.asset_get_calls, 2)

    def test_unchanged_nightly_still_public_is_an_idempotent_noop(self):
        api = FakeApi(
            release(tag_name="nightly", draft=False, prerelease=True),
            assets=self.live_assets,
        )
        result = self.recover(api)
        self.assertFalse(result.mutated)
        self.assertEqual(api.publish_calls, 0)

    def test_ambiguous_publish_response_retries_idempotently(self):
        api = FakeApi(
            release(tag_name="nightly", draft=True, prerelease=True),
            assets=self.live_assets,
            publish_then_failures=1,
        )
        result = self.recover(api)
        self.assertTrue(result.mutated)
        self.assertFalse(api.value["draft"])
        self.assertEqual(api.publish_calls, 1)

    def test_release_or_asset_drift_never_republishes(self):
        cases = {
            "release id": (release(id=701, tag_name="nightly", draft=True, prerelease=True), self.live_assets),
            "tag": (release(tag_name="moved", draft=True, prerelease=True), self.live_assets),
            "channel": (release(tag_name="nightly", draft=True, prerelease=False), self.live_assets),
            "immutable": (release(tag_name="nightly", draft=True, prerelease=True, immutable=True), self.live_assets),
            "asset id": (
                release(tag_name="nightly", draft=True, prerelease=True),
                [{**self.live_assets[0], "id": 99}],
            ),
            "asset digest": (
                release(tag_name="nightly", draft=True, prerelease=True),
                [{**self.live_assets[0], "digest": "sha256:" + "2" * 64}],
            ),
            "asset count": (
                release(tag_name="nightly", draft=True, prerelease=True),
                [{**self.live_assets[0], "download_count": 13}],
            ),
        }
        for label, (record, assets) in cases.items():
            with self.subTest(label=label):
                api = FakeApi(record, assets=assets)
                with self.assertRaises(RecoveryError):
                    self.recover(api)
                self.assertEqual(api.publish_calls, 0)

class ReleaseRecoveryWorkflowTests(unittest.TestCase):
    def test_direct_failure_recovery_requires_the_exact_staged_ledger_operation(self):
        workflow = (REPO_ROOT / ".github" / "workflows" / "release.yml").read_text(
            encoding="utf-8"
        )
        recovery_step = workflow.split(
            "    - name: Recover incomplete public release", maxsplit=1
        )[1]

        self.assertIn('--state-file "$STATE_FILE"', recovery_step)
        self.assertIn('--run-id "$GITHUB_RUN_ID"', recovery_step)
        self.assertIn('--run-attempt "$GITHUB_RUN_ATTEMPT"', recovery_step)
        self.assertIn('--source-sha "$GITHUB_SHA"', recovery_step)
        self.assertNotIn("--release-id", recovery_step)
        self.assertNotIn("steps.release-freeze.outputs.target_release_id", recovery_step)

    def test_nightly_snapshot_is_durable_before_hide_and_tag_move_waits_for_stage(self):
        workflow = (REPO_ROOT / ".github" / "workflows" / "release.yml").read_text(
            encoding="utf-8"
        )
        pending = workflow.index("    - name: Commit durable pending download-counter state")
        pre_hide = workflow.index(
            "    - name: Reverify exact public nightly snapshot before hiding"
        )
        hide = workflow.index("    - name: Hide and verify existing nightly release")
        nightly_stage = workflow.index("    - name: Stage nightly rolling release as draft")
        staged = workflow.index("    - name: Commit staged download-counter checkpoint")
        nightly_bind = workflow.index("    - name: Bind nightly tag to workflow commit")
        nightly_publish = workflow.index("    - name: Publish complete nightly rolling release")

        self.assertLess(pending, hide)
        self.assertLess(pending, pre_hide)
        self.assertLess(pre_hide, hide)
        self.assertLess(hide, nightly_stage)
        self.assertLess(staged, nightly_bind)
        self.assertLess(nightly_bind, nightly_publish)
        hide_step = workflow[hide:nightly_stage]
        self.assertIn("TARGET_WAS_DRAFT", hide_step)
        self.assertIn('if [[ "$TARGET_WAS_DRAFT" == "true" ]]', hide_step)
        self.assertIn('elif [[ "$TARGET_WAS_DRAFT" == "false" ]]', hide_step)


if __name__ == "__main__":
    unittest.main(verbosity=2)
