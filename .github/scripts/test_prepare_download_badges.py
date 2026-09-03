#!/usr/bin/env python3
"""Security and recovery tests for the schema-v2 download counter ledger."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("download_counter_ledger.py")
RELEASE_WORKFLOW = Path(__file__).resolve().parents[1] / "workflows" / "release.yml"
UPDATE_WORKFLOW = (
    Path(__file__).resolve().parents[1] / "workflows" / "update-downloads.yml"
)
README = Path(__file__).resolve().parents[2] / "README.md"
EXACT_EVIDENCE_SCRIPT = (
    Path(__file__).resolve().parents[2] / "tools" / "site-data" / "exact_evidence.py"
)
PUBLICATION_BOUNDARY_SCRIPT = Path(__file__).with_name(
    "verify-release-publication-boundary.sh"
)
ASSET_BOUNDARY_SCRIPT = Path(__file__).with_name(
    "verify_release_asset_boundary.py"
)
PUBLICATION_RECOVERY_SCRIPT = Path(__file__).with_name(
    "recover_release_publication.py"
)
SPEC = importlib.util.spec_from_file_location("download_counter_ledger", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
LEDGER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LEDGER
SPEC.loader.exec_module(LEDGER)

EXACT_SPEC = importlib.util.spec_from_file_location("exact_evidence", EXACT_EVIDENCE_SCRIPT)
assert EXACT_SPEC is not None and EXACT_SPEC.loader is not None
EXACT_EVIDENCE = importlib.util.module_from_spec(EXACT_SPEC)
EXACT_SPEC.loader.exec_module(EXACT_EVIDENCE)

RECOVERY_SPEC = importlib.util.spec_from_file_location(
    "recover_release_publication_for_ledger_tests", PUBLICATION_RECOVERY_SCRIPT
)
assert RECOVERY_SPEC is not None and RECOVERY_SPEC.loader is not None
RECOVERY = importlib.util.module_from_spec(RECOVERY_SPEC)
sys.modules[RECOVERY_SPEC.name] = RECOVERY
RECOVERY_SPEC.loader.exec_module(RECOVERY)

REPOSITORY = "Krilliac/SparkEngine"
SOURCE_SHA = "a" * 40
FIXED_NOW = lambda: datetime(2026, 8, 27, tzinfo=timezone.utc)


def asset_digest(name: str) -> str:
    return "sha256:" + hashlib.sha256(name.encode("utf-8")).hexdigest()


def ledger_asset(
    asset_id: int,
    release_id: int,
    name: str,
    count: int,
    digest: str | None = None,
    state: str = "uploaded",
):
    return LEDGER.AssetRecord(
        asset_id,
        release_id,
        name,
        count,
        (digest if digest is not None else asset_digest(name))
        if state == "uploaded"
        else None,
        state,
    )


def ledger_release(
    release_id: int, tag: str, *assets, draft=False, prerelease=None
):
    if prerelease is None:
        prerelease = tag == "nightly"
    return LEDGER.ReleaseRecord(
        release_id, tag, tuple(assets), draft, prerelease
    )


def ledger_inventory(*releases):
    return LEDGER.Inventory(tuple(releases))


class LedgerFiles:
    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.badges = self.root / "badges"
        self.data = self.badges / "downloads-data.json"
        self.expected = self.root / "expected-assets.txt"
        self.expected_digests = self.root / "expected-digests.txt"
        return self

    def __exit__(self, *args):
        self.temporary.cleanup()


def write_expected(files, names, digest_overrides=None):
    digest_overrides = digest_overrides or {}
    files.expected.write_text("".join(f"{name}\n" for name in names), encoding="utf-8")
    files.expected_digests.write_text(
        "".join(
            f"{digest_overrides.get(name, asset_digest(name)).removeprefix('sha256:')}  {name}\n"
            for name in names
        ),
        encoding="utf-8",
    )


def ledger_prepare(
    files,
    current,
    *,
    versioned=False,
    attempt="1",
    initialize=False,
    source_sha=SOURCE_SHA,
    target_tag=None,
    names=None,
    allow_exact_ci_evidence_replacement=False,
    digest_overrides=None,
):
    target_tag = target_tag or ("v1.2.3" if versioned else "nightly")
    if names is not None:
        write_expected(files, names, digest_overrides)
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.prepare_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            data_file=files.data,
            badge_directory=files.badges,
            api=object(),
            initialize=initialize,
            expected_assets_file=files.expected if names is not None else None,
            expected_digests_file=(
                files.expected_digests if names is not None else None
            ),
            allow_exact_ci_evidence_replacement=(
                allow_exact_ci_evidence_replacement
            ),
            now=FIXED_NOW,
        )


def ledger_replacement_preflight(
    files,
    current,
    names,
    *,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag="v1.2.3",
    digest_overrides=None,
):
    write_expected(files, names, digest_overrides)
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.preflight_evidence_replacement(
            repository=REPOSITORY,
            is_versioned=True,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            api=object(),
        )


def ledger_replacement_accept(
    files,
    current,
    names,
    *,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag="v1.2.3",
    digest_overrides=None,
):
    write_expected(files, names, digest_overrides)
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.accept_evidence_replacement(
            repository=REPOSITORY,
            is_versioned=True,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            badge_directory=files.badges,
            api=object(),
            now=FIXED_NOW,
        )


def ledger_cleanup_preflight(
    files,
    current,
    names,
    *,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag="v1.2.3",
    digest_overrides=None,
):
    write_expected(files, names, digest_overrides)
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.preflight_draft_cleanup(
            repository=REPOSITORY,
            is_versioned=True,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            api=object(),
        )


def ledger_cleanup_accept(
    files,
    current,
    names,
    *,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag="v1.2.3",
    digest_overrides=None,
):
    write_expected(files, names, digest_overrides)
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.accept_draft_cleanup(
            repository=REPOSITORY,
            is_versioned=True,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            badge_directory=files.badges,
            api=object(),
            now=FIXED_NOW,
        )


def ledger_stage(
    files,
    current,
    names,
    *,
    versioned=False,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag=None,
):
    write_expected(files, names)
    target_tag = target_tag or ("v1.2.3" if versioned else "nightly")
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.stage_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            api=object(),
            now=FIXED_NOW,
        )


def ledger_complete(
    files,
    current,
    names,
    *,
    versioned=False,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag=None,
):
    write_expected(files, names)
    target_tag = target_tag or ("v1.2.3" if versioned else "nightly")
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.complete_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            badge_directory=files.badges,
            api=object(),
            now=FIXED_NOW,
        )


def inventory_with_target_draft(current, target_tag, draft):
    return ledger_inventory(
        *(
            LEDGER.ReleaseRecord(
                release.release_id,
                release.tag_name,
                release.assets,
                draft if release.tag_name == target_tag else release.draft,
                release.prerelease,
            )
            for release in current.releases
        )
    )


def ledger_finalize(files, current, names, *, versioned=False, attempt="1"):
    """Exercise the staged checkpoint and published completion as one test helper."""
    target_tag = "v1.2.3" if versioned else "nightly"
    staged = inventory_with_target_draft(current, target_tag, True)
    ledger_stage(files, staged, names, versioned=versioned, attempt=attempt)
    published = inventory_with_target_draft(current, target_tag, False)
    return ledger_complete(
        files, published, names, versioned=versioned, attempt=attempt
    )


def ledger_preflight(
    files,
    current,
    names,
    *,
    versioned=False,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag=None,
    expect_target_draft=None,
):
    write_expected(files, names)
    target_tag = target_tag or ("v1.2.3" if versioned else "nightly")
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.preflight_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            api=object(),
            expect_target_draft=expect_target_draft,
        )


def ledger_publish_preflight(
    files,
    current,
    names,
    *,
    versioned=False,
    attempt="1",
    source_sha=SOURCE_SHA,
    target_tag=None,
    expect_draft=True,
):
    write_expected(files, names)
    target_tag = target_tag or ("v1.2.3" if versioned else "nightly")
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.prepublish_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=source_sha,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            data_file=files.data,
            api=object(),
            expect_draft=expect_draft,
        )


def ledger_inspect(
    files,
    current,
    names,
    *,
    versioned=False,
    target_tag=None,
    allow_exact_ci_evidence_replacement=False,
):
    write_expected(files, names)
    target_tag = target_tag or ("v1.2.3" if versioned else "nightly")
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.inspect_publication_target(
            repository=REPOSITORY,
            is_versioned=versioned,
            target_tag=target_tag,
            expected_assets_file=files.expected,
            expected_digests_file=files.expected_digests,
            api=object(),
            allow_exact_ci_evidence_replacement=(
                allow_exact_ci_evidence_replacement
            ),
        )


class V2StateAndRecoveryTests(unittest.TestCase):
    def test_missing_state_fails_closed_and_explicit_bootstrap_succeeds(self):
        with LedgerFiles() as files:
            with self.assertRaisesRegex(LEDGER.CounterError, "missing"):
                LEDGER.load_state(files.data)
            self.assertEqual(
                LEDGER.load_state(files.data, initialize=True),
                LEDGER.LegacyState(0, 0, 0, 0),
            )
            ledger_prepare(files, ledger_inventory(), initialize=True)
            state = LEDGER.load_state(files.data)
            self.assertEqual((state.total, state.installer_total), (0, 0))

    def test_malformed_duplicate_and_inconsistent_state_fail_closed(self):
        for payload in (b"{", b'{"total":1,"total":2}', b'{"schemaVersion":2}'):
            with self.subTest(payload=payload), LedgerFiles() as files:
                files.badges.mkdir()
                files.data.write_bytes(payload)
                with self.assertRaises(LEDGER.CounterError):
                    LEDGER.load_state(files.data)

    def test_legacy_migration_preserves_exact_totals(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "v1.0.0",
                ledger_asset(101, 10, "engine.zip", 55),
                ledger_asset(102, 10, "Engine-Setup.MSI", 12),
            )
        )
        with LedgerFiles() as files:
            files.badges.mkdir()
            files.data.write_text(
                json.dumps(
                    {
                        "total": 100,
                        "archivedNightly": 40,
                        "archivedNightlyInstallers": 10,
                        "installerTotal": 20,
                        "snapshotRunId": "old",
                    }
                ),
                encoding="utf-8",
            )
            result = ledger_prepare(files, current)
            state = LEDGER.load_state(files.data)
            self.assertEqual((result.total, result.installer_total), (107, 22))
            self.assertEqual((state.base_total, state.base_installer_total), (40, 10))
            self.assertEqual(set(state.assets), {101, 102})

    def test_exact_totals_and_msi_installer_classification(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "engine.zip", 7),
                ledger_asset(102, 10, "Engine-Setup.MSI", 3),
            ),
            ledger_release(
                20, "nightly", ledger_asset(201, 20, "Spark-Runtime.exe", 2)
            ),
        )
        with LedgerFiles() as files:
            prepared = ledger_prepare(files, current, versioned=True, initialize=True)
            names = ("engine.zip", "Engine-Setup.MSI")
            ledger_stage(files, current, names, versioned=True)
            final = ledger_complete(
                files, current, names, versioned=True
            )
            self.assertEqual((prepared.total, prepared.installer_total), (12, 5))
            self.assertEqual((final.total, final.installer_total), (12, 5))

    def test_scheduled_refresh_advances_lifetime_and_installer_badges(self):
        initial = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "engine.zip", 7),
                ledger_asset(102, 10, "Engine-Setup.MSI", 3),
            )
        )
        updated = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "engine.zip", 11),
                ledger_asset(102, 10, "Engine-Setup.MSI", 5),
            )
        )
        with LedgerFiles() as files:
            files.badges.mkdir()
            files.data.write_text('{"total": 0}\n', encoding="utf-8")
            with mock.patch.object(
                LEDGER, "fetch_consistent_inventory", return_value=initial
            ):
                first = LEDGER.refresh_counters(
                    repository=REPOSITORY,
                    data_file=files.data,
                    badge_directory=files.badges,
                    api=object(),
                    now=FIXED_NOW,
                )
            with mock.patch.object(
                LEDGER, "fetch_consistent_inventory", return_value=updated
            ):
                second = LEDGER.refresh_counters(
                    repository=REPOSITORY,
                    data_file=files.data,
                    badge_directory=files.badges,
                    api=object(),
                    now=FIXED_NOW,
                )

            self.assertEqual((first.total, first.installer_total), (10, 3))
            self.assertEqual((second.total, second.installer_total), (16, 5))
            self.assertEqual(
                json.loads((files.badges / "downloads.json").read_text())["message"],
                "16",
            )
            self.assertEqual(
                json.loads(
                    (files.badges / "installer-downloads.json").read_text()
                )["message"],
                "5",
            )

    def test_refresh_refuses_to_interleave_with_pending_publication(self):
        current = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, initialize=True)
            with mock.patch.object(
                LEDGER, "fetch_consistent_inventory", return_value=current
            ), self.assertRaisesRegex(LEDGER.CounterError, "pending recovery"):
                LEDGER.refresh_counters(
                    repository=REPOSITORY,
                    data_file=files.data,
                    badge_directory=files.badges,
                    api=object(),
                    now=FIXED_NOW,
                )

    def test_refresh_rejects_hidden_nightly_without_pending_state(self):
        hidden = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        with LedgerFiles() as files:
            files.badges.mkdir()
            files.data.write_text('{"total": 0}\n', encoding="utf-8")
            before = files.data.read_bytes()
            with mock.patch.object(
                LEDGER, "fetch_consistent_inventory", return_value=hidden
            ), self.assertRaisesRegex(LEDGER.CounterError, "hidden as a draft"):
                LEDGER.refresh_counters(
                    repository=REPOSITORY,
                    data_file=files.data,
                    badge_directory=files.badges,
                    api=object(),
                    now=FIXED_NOW,
                )
            self.assertEqual(files.data.read_bytes(), before)

    def test_nightly_replacement_keeps_old_high_water_once(self):
        old = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "old.zip", 9))
        )
        new = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(202, 10, "new.zip", 1))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, old, initialize=True)
            final = ledger_finalize(files, new, ("new.zip",))
            state = LEDGER.load_state(files.data)
            badge = json.loads((files.badges / "nightly-downloads.json").read_text())
            self.assertEqual(final.total, 10)
            self.assertEqual(
                {key: value.download_count for key, value in state.assets.items()},
                {101: 9, 202: 1},
            )
            self.assertEqual(badge["message"], "1")

    def test_delete_before_create_recovers_with_new_run_attempt(self):
        old = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "old.zip", 8))
        )
        new = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(202, 10, "new.zip", 0))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, old, initialize=True)
            recovered = ledger_prepare(files, ledger_inventory(), attempt="2")
            self.assertTrue(recovered.recovered_pending)
            self.assertEqual(recovered.operation_id, "700:2")
            self.assertEqual(recovered.total, 8)
            self.assertEqual(
                LEDGER.load_state(files.data).pending.prepared_asset_ids, frozenset()
            )
            self.assertEqual(LEDGER.load_state(files.data).pending.phase, "prepared")
            self.assertEqual(
                ledger_finalize(files, new, ("new.zip",), attempt="2").total, 8
            )

    def test_prepare_adopts_exact_same_target_and_source_under_new_attempt(self):
        current = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, initialize=True)
            recovered = ledger_prepare(files, current, attempt="2")
            state = LEDGER.load_state(files.data)
            self.assertTrue(recovered.recovered_pending)
            self.assertEqual(recovered.operation_id, "700:2")
            self.assertEqual(state.pending.operation_id, "700:2")
            self.assertEqual(state.pending.target_tag, "nightly")
            self.assertFalse(state.pending.is_versioned)
            self.assertEqual(state.pending.source_sha, SOURCE_SHA)
            self.assertEqual(state.pending.phase, "prepared")

    def test_same_source_retry_preserves_public_nightly_recovery_until_staged(self):
        names = ("one.zip",)
        public = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        hidden = inventory_with_target_draft(public, "nightly", True)

        with LedgerFiles() as files:
            ledger_prepare(files, public, initialize=True, names=names)
            first = LEDGER.load_state(files.data)
            self.assertIs(first.pending.target_draft_at_prepare, False)

            ledger_prepare(files, hidden, attempt="2", names=names)
            retried = LEDGER.load_state(files.data)
            self.assertIs(retried.pending.target_draft_at_prepare, False)
            self.assertEqual(retried.pending.prepared_asset_ids, frozenset({101}))

            prepared_target = RECOVERY.resolve_durable_recovery_target(
                retried.pending,
                assets=retried.assets,
                run_id=700,
                run_attempt=2,
                source_sha=SOURCE_SHA,
            )
            self.assertIsNotNone(prepared_target)
            self.assertEqual(prepared_target.action, "restore-public-nightly")
            self.assertEqual(
                prepared_target.expected_assets,
                (
                    RECOVERY.DurableAssetSnapshot(
                        101,
                        "one.zip",
                        4,
                        asset_digest("one.zip"),
                        "uploaded",
                    ),
                ),
            )

            ledger_stage(files, hidden, names, attempt="2")
            staged = LEDGER.load_state(files.data)
            staged_target = RECOVERY.resolve_durable_recovery_target(
                staged.pending,
                assets=staged.assets,
                run_id=700,
                run_attempt=2,
                source_sha=SOURCE_SHA,
            )
            self.assertEqual(staged_target.action, "redraft")

    def test_retry_does_not_inherit_public_origin_across_release_or_asset_drift(self):
        names = ("one.zip",)
        public = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        drifted = (
            ledger_inventory(
                ledger_release(
                    11,
                    "nightly",
                    ledger_asset(201, 11, "one.zip", 4),
                    draft=True,
                )
            ),
            ledger_inventory(
                ledger_release(
                    10,
                    "nightly",
                    ledger_asset(202, 10, "one.zip", 4),
                    draft=True,
                )
            ),
            ledger_inventory(
                ledger_release(
                    10,
                    "nightly",
                    ledger_asset(101, 10, "one.zip", 5),
                    draft=True,
                )
            ),
        )

        for current in drifted:
            with self.subTest(current=current), LedgerFiles() as files:
                ledger_prepare(files, public, initialize=True, names=names)
                ledger_prepare(files, current, attempt="2", names=names)
                retried = LEDGER.load_state(files.data)
                self.assertIs(retried.pending.target_draft_at_prepare, True)
                self.assertIsNone(
                    RECOVERY.resolve_durable_recovery_target(
                        retried.pending,
                        assets=retried.assets,
                        run_id=700,
                        run_attempt=2,
                        source_sha=SOURCE_SHA,
                    )
                )

    def test_prepare_rejects_cross_target_channel_or_source_pending(self):
        current = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        cases = (
            {
                "versioned": True,
                "target_tag": "v1.2.3",
                "source_sha": SOURCE_SHA,
                "label": "cross-channel",
            },
            {
                "versioned": False,
                "target_tag": "nightly",
                "source_sha": "b" * 40,
                "label": "same-nightly-new-source",
            },
        )
        for case in cases:
            with self.subTest(case=case["label"]), LedgerFiles() as files:
                ledger_prepare(files, current, initialize=True)
                before = files.data.read_bytes()
                with self.assertRaisesRegex(
                    LEDGER.CounterError, "different target, channel, or source SHA"
                ):
                    ledger_prepare(
                        files,
                        current,
                        attempt="2",
                        versioned=case["versioned"],
                        target_tag=case["target_tag"],
                        source_sha=case["source_sha"],
                    )
                self.assertEqual(files.data.read_bytes(), before)

    def test_prepare_rejects_different_version_tag_pending(self):
        current = ledger_inventory(
            ledger_release(
                10, "v1.2.3", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, versioned=True, initialize=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(
                LEDGER.CounterError, "different target, channel, or source SHA"
            ):
                ledger_prepare(
                    files,
                    current,
                    versioned=True,
                    attempt="2",
                    target_tag="v1.2.4",
                )
            self.assertEqual(files.data.read_bytes(), before)

    def test_publish_before_finalize_recovers_without_double_count(self):
        old = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "old.zip", 5))
        )
        published = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(202, 10, "new.zip", 1))
        )
        downloaded = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(202, 10, "new.zip", 2))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, old, initialize=True)
            recovered = ledger_prepare(files, published, attempt="2")
            self.assertTrue(recovered.recovered_pending)
            self.assertEqual(recovered.total, 6)
            final = ledger_finalize(files, downloaded, ("new.zip",), attempt="2")
            state = LEDGER.load_state(files.data)
            self.assertEqual(final.total, 7)
            self.assertEqual(state.assets[101].download_count, 5)
            self.assertEqual(state.assets[202].download_count, 2)
            self.assertIsNone(state.pending)

    def test_stage_preserves_pending_and_rewrites_staged_asset_ids(self):
        old = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 5), draft=True
            )
        )
        staged = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(202, 10, "one.zip", 0), draft=True
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, old, initialize=True)
            ledger_stage(files, staged, ("one.zip",))
            state = LEDGER.load_state(files.data)
            self.assertIsNotNone(state.pending)
            self.assertEqual(state.pending.phase, "staged")
            self.assertEqual(state.pending.prepared_asset_ids, frozenset({202}))
            self.assertEqual(state.live_asset_ids, frozenset({202}))
            self.assertEqual(state.assets[101].download_count, 5)
            self.assertFalse((files.badges / "nightly-downloads.json").exists())

    def test_complete_requires_published_exact_assets_before_clearing_pending(self):
        old = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 5), draft=True
            )
        )
        staged = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(202, 10, "one.zip", 0), draft=True
            )
        )
        published = inventory_with_target_draft(staged, "nightly", False)
        with LedgerFiles() as files:
            ledger_prepare(files, old, initialize=True)
            ledger_stage(files, staged, ("one.zip",))
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "remains a draft"):
                ledger_complete(files, staged, ("one.zip",))
            self.assertEqual(files.data.read_bytes(), before)

            wrong_digest = ledger_inventory(
                ledger_release(
                    10,
                    "nightly",
                    ledger_asset(202, 10, "one.zip", 0, "sha256:" + "f" * 64),
                )
            )
            with self.assertRaisesRegex(LEDGER.CounterError, "digest (mismatch|changed)"):
                ledger_complete(files, wrong_digest, ("one.zip",))
            self.assertEqual(files.data.read_bytes(), before)

            result = ledger_complete(files, published, ("one.zip",))
            state = LEDGER.load_state(files.data)
            self.assertEqual(result.total, 5)
            self.assertIsNone(state.pending)
            self.assertEqual(
                json.loads((files.badges / "downloads.json").read_text())["message"],
                "5",
            )
            self.assertEqual(
                json.loads((files.badges / "nightly-downloads.json").read_text())[
                    "message"
                ],
                "0",
            )

    def test_complete_rejects_post_stage_asset_set_change_without_writing(self):
        old = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 5), draft=True
            )
        )
        staged = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(202, 10, "one.zip", 0), draft=True
            )
        )
        changed = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(202, 10, "one.zip", 0),
                ledger_asset(203, 10, "unexpected.zip", 0),
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, old, initialize=True)
            ledger_stage(files, staged, ("one.zip",))
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "changed after staging"):
                ledger_complete(files, changed, ("one.zip",))
            self.assertEqual(files.data.read_bytes(), before)
            self.assertEqual(LEDGER.load_state(files.data).pending.phase, "staged")

    def test_complete_reconciles_downloads_recorded_during_publication(self):
        old = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 5), draft=True
            )
        )
        staged = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(202, 10, "one.zip", 0), draft=True
            )
        )
        published = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(202, 10, "one.zip", 3))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, old, initialize=True)
            ledger_stage(files, staged, ("one.zip",))
            result = ledger_complete(files, published, ("one.zip",))
            state = LEDGER.load_state(files.data)
            self.assertEqual(result.total, 8)
            self.assertEqual(state.assets[101].download_count, 5)
            self.assertEqual(state.assets[202].download_count, 3)
            self.assertIsNone(state.pending)

    def test_finalize_rejects_missing_and_unexpected_assets_exactly(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "one.zip", 1),
                ledger_asset(102, 10, "two.zip", 2),
            )
        )
        cases = (
            (("one.zip", "two.zip", "missing.zip"), "missing"),
            (("one.zip",), "unexpected"),
        )
        for names, message in cases:
            with self.subTest(names=names), LedgerFiles() as files:
                ledger_prepare(files, current, initialize=True)
                before = files.data.read_bytes()
                with self.assertRaisesRegex(LEDGER.CounterError, message):
                    ledger_finalize(files, current, names)
                self.assertEqual(files.data.read_bytes(), before)
                self.assertIsNotNone(LEDGER.load_state(files.data).pending)

    def test_preflight_allows_first_publication_exact_rerun_and_additive_nightly(self):
        for versioned in (False, True):
            tag = "v1.2.3" if versioned else "nightly"
            cases = (
                (ledger_inventory(), False, 0),
                (
                    ledger_inventory(
                        ledger_release(
                            10,
                            tag,
                            ledger_asset(101, 10, "one.zip", 4),
                            ledger_asset(102, 10, "two.zip", 7),
                            draft=not versioned,
                        )
                    ),
                    True,
                    2,
                ),
            )
            for current, exists, count in cases:
                with self.subTest(versioned=versioned, exists=exists), LedgerFiles() as files:
                    ledger_prepare(files, current, versioned=versioned, initialize=True)
                    before = files.data.read_bytes()
                    result = ledger_preflight(
                        files, current, ("one.zip", "two.zip"), versioned=versioned
                    )
                    self.assertEqual(
                        (result.target_exists, result.target_asset_count),
                        (exists, count),
                    )
                    self.assertEqual(files.data.read_bytes(), before)

        current = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, initialize=True)
            result = ledger_preflight(files, current, ("one.zip", "two.zip"))
            self.assertTrue(result.target_exists)
            self.assertEqual(result.target_asset_count, 1)

    def test_preflight_rejects_unexpected_nightly_and_any_versioned_mismatch(self):
        cases = (
            (
                False,
                ledger_inventory(
                    ledger_release(
                        10,
                        "nightly",
                        ledger_asset(101, 10, "one.zip", 4),
                        ledger_asset(102, 10, "obsolete.zip", 7),
                        draft=True,
                    )
                ),
                ("one.zip",),
                "unexpected assets: obsolete.zip",
            ),
            (
                True,
                ledger_inventory(
                    ledger_release(10, "v1.2.3", ledger_asset(101, 10, "one.zip", 4))
                ),
                ("one.zip", "two.zip"),
                "missing: two.zip",
            ),
            (
                True,
                ledger_inventory(
                    ledger_release(
                        10,
                        "v1.2.3",
                        ledger_asset(101, 10, "one.zip", 4),
                        ledger_asset(102, 10, "obsolete.zip", 7),
                    )
                ),
                ("one.zip",),
                "unexpected: obsolete.zip",
            ),
        )
        for versioned, current, expected, message in cases:
            with self.subTest(versioned=versioned, message=message), LedgerFiles() as files:
                ledger_prepare(files, current, versioned=versioned, initialize=True)
                before = files.data.read_bytes()
                with self.assertRaisesRegex(LEDGER.CounterError, message):
                    ledger_preflight(files, current, expected, versioned=versioned)
                self.assertEqual(files.data.read_bytes(), before)
                self.assertIsNotNone(LEDGER.load_state(files.data).pending)

    def test_finalize_rejects_mixed_revision_digests_before_publication(self):
        prepared = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "one.zip", 1),
                ledger_asset(102, 10, "two.zip", 2),
            )
        )
        mixed = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(201, 10, "one.zip", 0),
                ledger_asset(202, 10, "two.zip", 0, "sha256:" + "f" * 64),
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, prepared, initialize=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "digest mismatch: two.zip"):
                ledger_finalize(files, mixed, ("one.zip", "two.zip"))
            self.assertEqual(files.data.read_bytes(), before)
            self.assertIsNotNone(LEDGER.load_state(files.data).pending)

    def test_existing_versioned_release_requires_identical_digests(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4, "sha256:" + "f" * 64),
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, versioned=True, initialize=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "digest mismatch: one.zip"):
                ledger_preflight(files, current, ("one.zip",), versioned=True)

    def test_versioned_draft_digest_subset_is_resumable_without_asset_overwrite(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            inspected = ledger_inspect(
                files, current, ("one.zip", "two.zip"), versioned=True
            )
            self.assertEqual(inspected.target_release_id, 10)
            self.assertTrue(inspected.target_is_draft)
            ledger_prepare(files, current, versioned=True, initialize=True)
            result = ledger_preflight(
                files, current, ("one.zip", "two.zip"), versioned=True
            )
            self.assertEqual(result.target_release_id, 10)
            self.assertTrue(result.target_is_draft)

    def test_versioned_draft_subset_rejects_a_present_digest_mismatch(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, versioned=True, initialize=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "digest mismatch: one.zip"):
                ledger_preflight(
                    files, current, ("one.zip", "two.zip"), versioned=True
                )

    def test_inspect_allows_only_one_named_evidence_digest_to_change_on_stable_draft(self):
        evidence = "SparkEngine-Exact-CI-Evidence.json"
        current = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            inspected = ledger_inspect(
                files,
                current,
                ("one.zip", evidence),
                versioned=True,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertEqual(inspected.target_release_id, 10)
            self.assertTrue(inspected.target_is_draft)

            with self.assertRaisesRegex(LEDGER.CounterError, "digest mismatch: one.zip"):
                other_mismatch = ledger_inventory(
                    ledger_release(
                        10,
                        "v1.2.3",
                        ledger_asset(101, 10, "one.zip", 4, "sha256:" + "e" * 64),
                        ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
                        draft=True,
                    )
                )
                ledger_inspect(
                    files,
                    other_mismatch,
                    ("one.zip", evidence),
                    versioned=True,
                    allow_exact_ci_evidence_replacement=True,
                )

    def test_replaceable_draft_asset_never_relaxes_published_or_nightly_state(self):
        evidence = "SparkEngine-Exact-CI-Evidence.json"
        published = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
            )
        )
        with LedgerFiles() as files:
            with self.assertRaisesRegex(LEDGER.CounterError, "digest (mismatch|changed)"):
                ledger_inspect(
                    files,
                    published,
                    (evidence,),
                    versioned=True,
                    allow_exact_ci_evidence_replacement=True,
                )
            with self.assertRaisesRegex(LEDGER.CounterError, "only for versioned"):
                ledger_inspect(
                    files,
                    ledger_inventory(),
                    (evidence,),
                    allow_exact_ci_evidence_replacement=True,
                )

    def test_replaceable_draft_asset_is_fixed_and_must_be_expected(self):
        current = ledger_inventory(
            ledger_release(10, "v1.2.3", draft=True)
        )
        with LedgerFiles() as files:
            with self.assertRaisesRegex(LEDGER.CounterError, "exact expected asset"):
                ledger_inspect(
                    files,
                    current,
                    ("one.zip",),
                    versioned=True,
                    allow_exact_ci_evidence_replacement=True,
                )

    def test_stable_draft_evidence_replacement_is_durable_and_preserves_counts(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        old = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 7, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        deleted = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                draft=True,
            )
        )
        uploaded = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(202, 10, evidence, 0),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            prepared = ledger_prepare(
                files,
                old,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertTrue(prepared.replacement_required)
            state = LEDGER.load_state(files.data)
            self.assertEqual(state.pending.evidence_replacement.old_asset_id, 102)
            self.assertEqual(state.pending.evidence_replacement.old_digest, "sha256:" + "f" * 64)
            self.assertEqual(
                state.pending.evidence_replacement.expected_digest,
                asset_digest(evidence),
            )

            before_delete = ledger_replacement_preflight(files, old, names)
            self.assertTrue(before_delete.delete_required)
            self.assertTrue(before_delete.upload_required)
            self.assertEqual(before_delete.delete_asset_id, 102)
            self.assertEqual(before_delete.delete_asset_digest, "sha256:" + "f" * 64)
            self.assertEqual(before_delete.delete_asset_state, "uploaded")
            self.assertEqual(before_delete.delete_asset_download_count, 7)

            after_delete = ledger_replacement_preflight(files, deleted, names)
            self.assertFalse(after_delete.delete_required)
            self.assertTrue(after_delete.upload_required)
            with self.assertRaisesRegex(LEDGER.CounterError, "not fully uploaded"):
                ledger_replacement_accept(files, deleted, names)

            after_upload = ledger_replacement_preflight(files, uploaded, names)
            self.assertFalse(after_upload.delete_required)
            self.assertFalse(after_upload.upload_required)
            accepted = ledger_replacement_accept(files, uploaded, names)
            self.assertFalse(accepted.replacement_required)
            accepted_state = LEDGER.load_state(files.data)
            self.assertIsNone(accepted_state.pending.evidence_replacement)
            self.assertEqual(accepted_state.pending.prepared_asset_ids, frozenset({101, 202}))
            self.assertIs(accepted_state.pending.target_draft_at_prepare, True)
            self.assertEqual(accepted_state.total, 11)
            self.assertEqual(accepted_state.assets[102].download_count, 7)
            ledger_preflight(files, uploaded, names, versioned=True)

    def test_evidence_replacement_recovers_after_delete_and_after_upload(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        old = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 7, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        deleted = ledger_inventory(
            ledger_release(10, "v1.2.3", ledger_asset(101, 10, "one.zip", 5), draft=True)
        )
        uploaded = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 6),
                ledger_asset(202, 10, evidence, 0),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                old,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            recovered_deleted = ledger_prepare(
                files,
                deleted,
                versioned=True,
                attempt="2",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertTrue(recovered_deleted.recovered_pending)
            self.assertTrue(recovered_deleted.replacement_required)
            self.assertTrue(
                ledger_replacement_preflight(files, deleted, names, attempt="2").upload_required
            )

            recovered_uploaded = ledger_prepare(
                files,
                uploaded,
                versioned=True,
                attempt="3",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertTrue(recovered_uploaded.replacement_required)
            ready = ledger_replacement_preflight(files, uploaded, names, attempt="3")
            self.assertFalse(ready.delete_required)
            self.assertFalse(ready.upload_required)
            ledger_replacement_accept(files, uploaded, names, attempt="3")
            state = LEDGER.load_state(files.data)
            self.assertEqual(state.total, 13)
            self.assertEqual(state.assets[102].download_count, 7)

    def test_evidence_replacement_rejects_a_reused_historical_asset_id(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        old = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
                draft=True,
            ),
            ledger_release(
                20, "v0.9.0", ledger_asset(202, 20, "retired.zip", 3)
            ),
        )
        reused = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(202, 10, evidence, 0),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                old,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            with self.assertRaisesRegex(LEDGER.CounterError, "historical asset ID"):
                ledger_replacement_preflight(files, reused, names)
            with self.assertRaisesRegex(LEDGER.CounterError, "historical asset ID"):
                ledger_replacement_accept(files, reused, names)

    def test_unresolved_replacement_blocks_normal_preflight_stage_and_foreign_drift(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        old = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        drifted = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(999, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                old,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            with self.assertRaisesRegex(LEDGER.CounterError, "accepted exact CI evidence"):
                ledger_preflight(files, old, names, versioned=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "accepted exact CI evidence"):
                ledger_stage(files, old, names, versioned=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "non-evidence draft asset IDs"):
                ledger_replacement_preflight(files, drifted, names)

    def test_stable_visibility_release_identity_and_existing_asset_ids_are_frozen(self):
        names = ("one.zip",)
        draft = ledger_inventory(
            ledger_release(
                10, "v1.2.3", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        published = inventory_with_target_draft(draft, "v1.2.3", False)

        for initial, changed in ((draft, published), (published, draft)):
            with self.subTest(
                initial_draft=initial.releases[0].draft
            ), LedgerFiles() as files:
                ledger_prepare(files, initial, versioned=True, initialize=True)
                before = files.data.read_bytes()
                with self.assertRaisesRegex(LEDGER.CounterError, "draft state changed"):
                    ledger_preflight(files, changed, names, versioned=True)
                self.assertEqual(files.data.read_bytes(), before)
                with self.assertRaisesRegex(LEDGER.CounterError, "draft state changed"):
                    ledger_stage(files, changed, names, versioned=True)
                self.assertEqual(files.data.read_bytes(), before)

        recreated = ledger_inventory(
            ledger_release(
                11, "v1.2.3", ledger_asset(201, 11, "one.zip", 4), draft=True
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, draft, versioned=True, initialize=True)
            ledger_preflight(files, draft, names, versioned=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "identity changed"):
                ledger_stage(files, recreated, names, versioned=True)
            self.assertEqual(files.data.read_bytes(), before)

        replaced_asset = ledger_inventory(
            ledger_release(
                10, "v1.2.3", ledger_asset(202, 10, "one.zip", 4), draft=True
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, draft, versioned=True, initialize=True)
            ledger_preflight(files, draft, names, versioned=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "replaced an existing asset"):
                ledger_stage(files, replaced_asset, names, versioned=True)
            self.assertEqual(files.data.read_bytes(), before)

    def test_new_stable_draft_and_published_stable_noop_preserve_visibility_contract(self):
        names = ("one.zip",)
        new_draft = ledger_inventory(
            ledger_release(
                10, "v1.2.3", ledger_asset(101, 10, "one.zip", 0), draft=True
            )
        )
        published = inventory_with_target_draft(new_draft, "v1.2.3", False)

        with LedgerFiles() as files:
            ledger_prepare(files, ledger_inventory(), versioned=True, initialize=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "changed after the durable"):
                ledger_preflight(files, new_draft, names, versioned=True)

        with LedgerFiles() as files:
            ledger_prepare(files, ledger_inventory(), versioned=True, initialize=True)
            absent = ledger_preflight(
                files, ledger_inventory(), names, versioned=True
            )
            self.assertFalse(absent.target_exists)
            ledger_stage(files, new_draft, names, versioned=True)
            staged = LEDGER.load_state(files.data)
            self.assertIsNone(staged.pending.target_draft_at_prepare)
            self.assertEqual(staged.pending.target_release_id, 10)
            ledger_complete(files, published, names, versioned=True)

        with LedgerFiles() as files:
            ledger_prepare(files, published, versioned=True, initialize=True)
            preflight = ledger_preflight(files, published, names, versioned=True)
            self.assertFalse(preflight.target_is_draft)
            ledger_stage(files, published, names, versioned=True)
            completed = ledger_complete(files, published, names, versioned=True)
            self.assertEqual(completed.target_downloads, 0)

    def test_pending_target_draft_snapshot_round_trips_and_rejects_hostile_values(self):
        cases = (
            (ledger_inventory(), None),
            (
                ledger_inventory(
                    ledger_release(
                        10,
                        "v1.2.3",
                        ledger_asset(101, 10, "one.zip", 0),
                        draft=True,
                    )
                ),
                True,
            ),
            (
                ledger_inventory(
                    ledger_release(
                        10, "v1.2.3", ledger_asset(101, 10, "one.zip", 0)
                    )
                ),
                False,
            ),
        )
        for inventory, expected in cases:
            with self.subTest(expected=expected), LedgerFiles() as files:
                ledger_prepare(files, inventory, versioned=True, initialize=True)
                self.assertIs(
                    LEDGER.load_state(files.data).pending.target_draft_at_prepare,
                    expected,
                )

        published = cases[2][0]
        with LedgerFiles() as files:
            ledger_prepare(files, published, versioned=True, initialize=True)
            original = json.loads(files.data.read_text(encoding="utf-8"))
            hostile = (
                ("missing", object()),
                ("string", "true"),
                ("zero", 0),
                ("one", 1),
                ("null-existing", None),
            )
            for label, value in hostile:
                with self.subTest(label=label):
                    payload = json.loads(json.dumps(original))
                    if label == "missing":
                        payload["pending"].pop("targetDraftAtPrepare")
                    else:
                        payload["pending"]["targetDraftAtPrepare"] = value
                    files.data.write_text(json.dumps(payload), encoding="utf-8")
                    with self.assertRaises(LEDGER.CounterError):
                        LEDGER.load_state(files.data)

        with LedgerFiles() as files:
            ledger_prepare(
                files, ledger_inventory(), versioned=True, initialize=True
            )
            payload = json.loads(files.data.read_text(encoding="utf-8"))
            payload["pending"]["targetDraftAtPrepare"] = True
            files.data.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(LEDGER.CounterError, "must be null"):
                LEDGER.load_state(files.data)

    def test_existing_schema_v2_state_without_new_asset_fields_replays(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "one.zip", 4),
                draft=True,
                prerelease=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, initialize=True)
            payload = json.loads(files.data.read_text(encoding="utf-8"))
            for asset in payload["assets"]:
                asset.pop("digest")
                asset.pop("state")
            payload["pending"].pop("draftCleanup")
            files.data.write_text(json.dumps(payload), encoding="utf-8")
            loaded = LEDGER.load_state(files.data)
            self.assertEqual(loaded.assets[101].state, "uploaded")
            self.assertIsNone(loaded.assets[101].digest)
            recovered = ledger_prepare(files, current, attempt="2")
            self.assertTrue(recovered.recovered_pending)
            replayed = LEDGER.load_state(files.data)
            self.assertEqual(replayed.assets[101].digest, asset_digest("one.zip"))
            self.assertEqual(replayed.assets[101].state, "uploaded")

    def test_versioned_draft_subset_rejects_an_unexpected_present_asset(self):
        current = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "obsolete.zip", 4),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            with self.assertRaisesRegex(LEDGER.CounterError, "unexpected assets"):
                ledger_inspect(files, current, ("one.zip",), versioned=True)

    def test_inspect_allows_published_additive_nightly_before_explicit_hide(self):
        current = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        with LedgerFiles() as files:
            result = ledger_inspect(files, current, ("one.zip", "two.zip"))
            self.assertEqual(result.target_release_id, 10)
            self.assertFalse(result.target_is_draft)

    def test_preflight_requires_existing_nightly_to_be_hidden(self):
        current = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, initialize=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "not draft"):
                ledger_preflight(files, current, ("one.zip",))

    def test_pre_hide_preflight_requires_the_exact_public_nightly_snapshot(self):
        public = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        hidden = inventory_with_target_draft(public, "nightly", True)
        changed = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(202, 10, "one.zip", 4))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, public, initialize=True)
            result = ledger_preflight(
                files, public, ("one.zip",), expect_target_draft=False
            )
            self.assertFalse(result.target_is_draft)
            for label, inventory in (("hidden", hidden), ("changed", changed)):
                with self.subTest(label=label), self.assertRaises(LEDGER.CounterError):
                    ledger_preflight(
                        files,
                        inventory,
                        ("one.zip",),
                        expect_target_draft=False,
                    )

    def test_preflight_rejects_inventory_change_after_durable_snapshot(self):
        prepared = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        changed = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(202, 10, "one.zip", 4))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, prepared, initialize=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "changed after"):
                ledger_preflight(files, changed, ("one.zip",))
            self.assertEqual(files.data.read_bytes(), before)

    def test_preflight_rejects_download_count_growth_after_snapshot(self):
        prepared = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        changed = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 5), draft=True
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, prepared, initialize=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "download counts changed"):
                ledger_preflight(files, changed, ("one.zip",))
            self.assertEqual(files.data.read_bytes(), before)

    def test_rerun_after_count_growth_preserves_the_new_high_water(self):
        prepared = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        changed = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(101, 10, "one.zip", 5), draft=True
            )
        )
        staged = ledger_inventory(
            ledger_release(
                10, "nightly", ledger_asset(202, 10, "one.zip", 0), draft=True
            )
        )
        published = inventory_with_target_draft(staged, "nightly", False)
        with LedgerFiles() as files:
            ledger_prepare(files, prepared, initialize=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "download counts changed"):
                ledger_preflight(files, changed, ("one.zip",))
            recovered = ledger_prepare(files, changed, attempt="2")
            self.assertTrue(recovered.recovered_pending)
            ledger_preflight(files, changed, ("one.zip",), attempt="2")
            ledger_stage(files, staged, ("one.zip",), attempt="2")
            result = ledger_complete(files, published, ("one.zip",), attempt="2")
            self.assertEqual(result.total, 5)
            self.assertEqual(
                LEDGER.load_state(files.data).assets[101].download_count, 5
            )

    def test_preflight_requires_the_exact_pending_operation(self):
        current = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, initialize=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "requires this operation"):
                ledger_preflight(files, current, ("one.zip",), attempt="2")
            self.assertEqual(files.data.read_bytes(), before)


    def test_first_stage_partial_upload_cleanup_is_durable_and_crash_safe(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", "two.zip", evidence)
        valid = ledger_asset(201, 10, "one.zip", 0)
        starter = ledger_asset(202, 10, evidence, 7, state="starter")
        uploaded_without_digest = LEDGER.AssetRecord(
            203, 10, "two.zip", 3, None, "uploaded"
        )
        partial = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                valid,
                starter,
                uploaded_without_digest,
                draft=True,
            )
        )
        after_one_delete = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                valid,
                uploaded_without_digest,
                draft=True,
            )
        )
        cleaned = ledger_inventory(
            ledger_release(10, "v1.2.3", valid, draft=True)
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                ledger_inventory(),
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            recovered = ledger_prepare(
                files,
                partial,
                versioned=True,
                attempt="2",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertTrue(recovered.cleanup_required)
            self.assertFalse(recovered.replacement_required)
            state = LEDGER.load_state(files.data)
            self.assertEqual(state.pending.prepared_asset_ids, frozenset({201}))
            self.assertEqual(
                tuple(cleanup.asset_id for cleanup in state.pending.draft_cleanup),
                (202, 203),
            )
            self.assertEqual(LEDGER.load_state(files.data), state)
            first_plan = ledger_cleanup_preflight(
                files, partial, names, attempt="2"
            )
            self.assertEqual(
                tuple(cleanup.asset_id for cleanup in first_plan.cleanup_assets),
                (202, 203),
            )

            rerun = ledger_prepare(
                files,
                after_one_delete,
                versioned=True,
                attempt="3",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertTrue(rerun.cleanup_required)
            second_plan = ledger_cleanup_preflight(
                files, after_one_delete, names, attempt="3"
            )
            self.assertEqual(
                tuple(cleanup.asset_id for cleanup in second_plan.cleanup_assets),
                (203,),
            )

            empty_plan = ledger_cleanup_preflight(
                files, cleaned, names, attempt="3"
            )
            self.assertEqual(empty_plan.cleanup_assets, ())
            accepted = ledger_cleanup_accept(
                files, cleaned, names, attempt="3"
            )
            self.assertFalse(accepted.cleanup_required)
            accepted_state = LEDGER.load_state(files.data)
            self.assertEqual(accepted_state.pending.draft_cleanup, ())
            self.assertEqual(accepted_state.pending.prepared_asset_ids, frozenset({201}))
            self.assertEqual(accepted_state.live_asset_ids, frozenset({201}))
            self.assertTrue({202, 203}.issubset(accepted_state.assets))
            self.assertEqual(accepted_state.total, 10)
            ledger_preflight(files, cleaned, names, versioned=True, attempt="3")

    def test_cleanup_freezes_existing_ids_and_rejects_unexpected_candidates(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", "two.zip", evidence)
        baseline = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 0),
                draft=True,
            )
        )
        partial = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 0),
                ledger_asset(202, 10, "two.zip", 0, state="starter"),
                draft=True,
            )
        )
        missing_baseline = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(202, 10, "two.zip", 0, state="starter"),
                draft=True,
            )
        )
        unexpected = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 0),
                ledger_asset(303, 10, "obsolete.zip", 0, state="starter"),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                baseline,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            prepared = ledger_prepare(
                files,
                partial,
                versioned=True,
                attempt="2",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertTrue(prepared.cleanup_required)
            state = LEDGER.load_state(files.data)
            self.assertEqual(state.pending.prepared_asset_ids, frozenset({101}))
            self.assertEqual(state.pending.draft_cleanup[0].asset_id, 202)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "101 disappeared"):
                ledger_prepare(
                    files,
                    missing_baseline,
                    versioned=True,
                    attempt="3",
                    names=names,
                    allow_exact_ci_evidence_replacement=True,
                )
            self.assertEqual(files.data.read_bytes(), before)
            with self.assertRaisesRegex(LEDGER.CounterError, "unexpected assets"):
                ledger_prepare(
                    files,
                    unexpected,
                    versioned=True,
                    attempt="3",
                    names=names,
                    allow_exact_ci_evidence_replacement=True,
                )
            self.assertEqual(files.data.read_bytes(), before)

    def test_cleanup_preflight_rejects_a_forged_exact_asset_intent(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", "two.zip", evidence)
        partial = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(201, 10, "one.zip", 0),
                ledger_asset(202, 10, "two.zip", 0, state="starter"),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                ledger_inventory(),
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            ledger_prepare(
                files,
                partial,
                versioned=True,
                attempt="2",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            payload = json.loads(files.data.read_text(encoding="utf-8"))
            payload["pending"]["preparedAssetIds"] = []
            payload["pending"]["draftCleanup"] = [
                {
                    "assetId": 201,
                    "name": "one.zip",
                    "digest": asset_digest("one.zip"),
                    "state": "uploaded",
                    "downloadCount": 0,
                }
            ]
            files.data.write_text(json.dumps(payload), encoding="utf-8")
            before = files.data.read_bytes()
            only_exact = ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    ledger_asset(201, 10, "one.zip", 0),
                    draft=True,
                )
            )
            with self.assertRaisesRegex(LEDGER.CounterError, "cannot delete an exact asset"):
                ledger_cleanup_preflight(files, only_exact, names, attempt="2")
            self.assertEqual(files.data.read_bytes(), before)

    def test_inspect_recovery_mode_is_read_only_and_prepare_stays_strict(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        starter = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(201, 10, "one.zip", 0, state="starter"),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            write_expected(files, names)
            with mock.patch.object(
                LEDGER, "fetch_consistent_inventory", return_value=starter
            ):
                inspected = LEDGER.inspect_publication_target(
                    repository=REPOSITORY,
                    is_versioned=True,
                    target_tag="v1.2.3",
                    expected_assets_file=files.expected,
                    expected_digests_file=files.expected_digests,
                    api=object(),
                    allow_exact_ci_evidence_replacement=True,
                    allow_stable_draft_recovery_candidates=True,
                )
            self.assertTrue(inspected.target_exists)
            with self.assertRaisesRegex(LEDGER.CounterError, "asset digests differ"):
                ledger_prepare(
                    files,
                    starter,
                    versioned=True,
                    initialize=True,
                    names=names,
                    allow_exact_ci_evidence_replacement=True,
                )
            self.assertFalse(files.data.exists())

    def test_cleanup_and_evidence_replacement_are_mutually_exclusive(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", "two.zip", evidence)
        old = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 0),
                ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        mixed_live = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 0),
                ledger_asset(102, 10, evidence, 0, "sha256:" + "f" * 64),
                ledger_asset(203, 10, "two.zip", 0, state="starter"),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                old,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "cleanup must be accepted"):
                ledger_prepare(
                    files,
                    mixed_live,
                    versioned=True,
                    attempt="2",
                    names=names,
                    allow_exact_ci_evidence_replacement=True,
                )
            self.assertEqual(files.data.read_bytes(), before)

            payload = json.loads(before)
            payload["assets"].append(
                {
                    "id": 203,
                    "releaseId": 10,
                    "name": "two.zip",
                    "downloadCount": 0,
                    "digest": None,
                    "state": "starter",
                }
            )
            payload["liveAssetIds"].append(203)
            payload["pending"]["draftCleanup"] = [
                {
                    "assetId": 203,
                    "name": "two.zip",
                    "downloadCount": 0,
                    "digest": None,
                    "state": "starter",
                }
            ]
            files.data.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(LEDGER.CounterError, "cannot mix"):
                LEDGER.load_state(files.data)

    def test_cleanup_preflight_rejects_tuple_and_release_state_drift(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        partial = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(201, 10, "one.zip", 2, state="starter"),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                ledger_inventory(),
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            ledger_prepare(
                files,
                partial,
                versioned=True,
                attempt="2",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            before = files.data.read_bytes()
            drifted_count = ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    ledger_asset(201, 10, "one.zip", 3, state="starter"),
                    draft=True,
                )
            )
            cases = {
                "tuple": drifted_count,
                "published": ledger_inventory(
                    ledger_release(
                        10,
                        "v1.2.3",
                        ledger_asset(201, 10, "one.zip", 2, state="starter"),
                        draft=False,
                    )
                ),
                "prerelease": ledger_inventory(
                    ledger_release(
                        10,
                        "v1.2.3",
                        ledger_asset(201, 10, "one.zip", 2, state="starter"),
                        draft=True,
                        prerelease=True,
                    )
                ),
            }
            for label, inventory in cases.items():
                with self.subTest(label=label), self.assertRaises(LEDGER.CounterError):
                    ledger_cleanup_preflight(files, inventory, names, attempt="2")
                self.assertEqual(files.data.read_bytes(), before)

    def test_failed_upload_starter_is_durably_cleaned_and_retried(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        old = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 7, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        deleted = ledger_inventory(
            ledger_release(
                10, "v1.2.3", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        starter = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(202, 10, evidence, 0, state="starter"),
                draft=True,
            )
        )
        uploaded = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(203, 10, evidence, 0),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                old,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            ledger_prepare(
                files,
                deleted,
                versioned=True,
                attempt="2",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            ledger_prepare(
                files,
                starter,
                versioned=True,
                attempt="3",
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            state = LEDGER.load_state(files.data)
            self.assertEqual(
                state.pending.evidence_replacement.cleanup,
                LEDGER.EvidenceCleanupIntent(202, None, "starter", 0),
            )
            before_preflight = files.data.read_bytes()
            authorized = ledger_replacement_preflight(
                files, starter, names, attempt="3"
            )
            self.assertTrue(authorized.delete_required)
            self.assertTrue(authorized.upload_required)
            self.assertEqual(authorized.delete_asset_id, 202)
            self.assertIsNone(authorized.delete_asset_digest)
            self.assertEqual(authorized.delete_asset_state, "starter")
            self.assertEqual(files.data.read_bytes(), before_preflight)

            after_cleanup = ledger_replacement_preflight(
                files, deleted, names, attempt="3"
            )
            self.assertFalse(after_cleanup.delete_required)
            self.assertTrue(after_cleanup.upload_required)
            ledger_replacement_accept(files, uploaded, names, attempt="3")
            accepted = LEDGER.load_state(files.data)
            self.assertIsNone(accepted.pending.evidence_replacement)
            self.assertEqual(accepted.pending.prepared_asset_ids, frozenset({101, 203}))
            self.assertEqual(accepted.assets[102].download_count, 7)
            self.assertEqual(accepted.assets[202].state, "starter")
            self.assertEqual(accepted.total, 11)

    def test_replacement_digest_can_be_superseded_after_old_asset_deletion(self):
        evidence = LEDGER.EXACT_CI_EVIDENCE_ASSET
        names = ("one.zip", evidence)
        digest_a = asset_digest(evidence)
        digest_b = "sha256:" + "b" * 64
        overrides = {evidence: digest_b}
        old = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, evidence, 7, "sha256:" + "f" * 64),
                draft=True,
            )
        )
        deleted = ledger_inventory(
            ledger_release(
                10, "v1.2.3", ledger_asset(101, 10, "one.zip", 4), draft=True
            )
        )
        outdated = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(202, 10, evidence, 0, digest_a),
                draft=True,
            )
        )
        uploaded_b = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(203, 10, evidence, 0, digest_b),
                draft=True,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(
                files,
                old,
                versioned=True,
                initialize=True,
                names=names,
                allow_exact_ci_evidence_replacement=True,
            )
            ledger_prepare(
                files,
                deleted,
                versioned=True,
                attempt="2",
                names=names,
                digest_overrides=overrides,
                allow_exact_ci_evidence_replacement=True,
            )
            self.assertEqual(
                LEDGER.load_state(files.data).pending.evidence_replacement.expected_digest,
                digest_b,
            )
            ledger_prepare(
                files,
                outdated,
                versioned=True,
                attempt="3",
                names=names,
                digest_overrides=overrides,
                allow_exact_ci_evidence_replacement=True,
            )
            authorized = ledger_replacement_preflight(
                files,
                outdated,
                names,
                attempt="3",
                digest_overrides=overrides,
            )
            self.assertEqual(authorized.delete_asset_id, 202)
            self.assertEqual(authorized.delete_asset_digest, digest_a)
            self.assertEqual(authorized.delete_asset_state, "uploaded")
            after_delete = ledger_replacement_preflight(
                files,
                deleted,
                names,
                attempt="3",
                digest_overrides=overrides,
            )
            self.assertTrue(after_delete.upload_required)
            ledger_replacement_accept(
                files,
                uploaded_b,
                names,
                attempt="3",
                digest_overrides=overrides,
            )
            accepted = LEDGER.load_state(files.data)
            self.assertIsNone(accepted.pending.evidence_replacement)
            self.assertEqual(accepted.assets[102].download_count, 7)
            self.assertEqual(accepted.total, 11)

    def test_publish_preflight_rejects_post_stage_mutation_without_writing(self):
        names = ("one.zip", "two.zip")
        staged = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, "two.zip", 2),
                draft=True,
            )
        )
        variants = {
            "added": ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    *staged.releases[0].assets,
                    ledger_asset(103, 10, "extra.zip", 0),
                    draft=True,
                )
            ),
            "replaced ID": ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    ledger_asset(201, 10, "one.zip", 4),
                    ledger_asset(102, 10, "two.zip", 2),
                    draft=True,
                )
            ),
            "changed digest": ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    ledger_asset(101, 10, "one.zip", 4, "sha256:" + "f" * 64),
                    ledger_asset(102, 10, "two.zip", 2),
                    draft=True,
                )
            ),
            "changed count": ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    ledger_asset(101, 10, "one.zip", 5),
                    ledger_asset(102, 10, "two.zip", 2),
                    draft=True,
                )
            ),
            "published early": inventory_with_target_draft(staged, "v1.2.3", False),
            "wrong prerelease": ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    *staged.releases[0].assets,
                    draft=True,
                    prerelease=True,
                )
            ),
        }
        with LedgerFiles() as files:
            ledger_prepare(
                files, ledger_inventory(), versioned=True, initialize=True, names=names
            )
            ledger_stage(files, staged, names, versioned=True)
            frozen = files.data.read_bytes()
            ledger_publish_preflight(files, staged, names, versioned=True)
            self.assertEqual(files.data.read_bytes(), frozen)
            for label, changed in variants.items():
                with self.subTest(label=label), self.assertRaises(LEDGER.CounterError):
                    ledger_publish_preflight(files, changed, names, versioned=True)
                self.assertEqual(files.data.read_bytes(), frozen)

    def test_post_publish_preflight_requires_the_exact_public_release(self):
        names = ("one.zip", "two.zip")
        staged = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                ledger_asset(102, 10, "two.zip", 2),
                draft=True,
            )
        )
        published = inventory_with_target_draft(staged, "v1.2.3", False)
        with LedgerFiles() as files:
            ledger_prepare(
                files, ledger_inventory(), versioned=True, initialize=True, names=names
            )
            ledger_stage(files, staged, names, versioned=True)
            frozen = files.data.read_bytes()
            result = ledger_publish_preflight(
                files,
                published,
                names,
                versioned=True,
                expect_draft=False,
            )
            self.assertTrue(result.target_exists)
            self.assertFalse(result.target_is_draft)
            self.assertEqual(files.data.read_bytes(), frozen)

            growth = ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    ledger_asset(101, 10, "one.zip", 5),
                    ledger_asset(102, 10, "two.zip", 3),
                    draft=False,
                )
            )
            ledger_publish_preflight(
                files,
                growth,
                names,
                versioned=True,
                expect_draft=False,
            )
            self.assertEqual(files.data.read_bytes(), frozen)

            regression = ledger_inventory(
                ledger_release(
                    10,
                    "v1.2.3",
                    ledger_asset(101, 10, "one.zip", 3),
                    ledger_asset(102, 10, "two.zip", 2),
                    draft=False,
                )
            )
            with self.assertRaisesRegex(LEDGER.CounterError, "download count"):
                ledger_publish_preflight(
                    files,
                    regression,
                    names,
                    versioned=True,
                    expect_draft=False,
                )
            self.assertEqual(files.data.read_bytes(), frozen)

            with self.assertRaisesRegex(LEDGER.CounterError, "returned to draft"):
                ledger_publish_preflight(
                    files,
                    staged,
                    names,
                    versioned=True,
                    expect_draft=False,
                )
            self.assertEqual(files.data.read_bytes(), frozen)

    def test_channel_prerelease_state_is_fail_closed_after_initial_inspect(self):
        names = ("one.zip",)
        bad_stable = ledger_inventory(
            ledger_release(
                10,
                "v1.2.3",
                ledger_asset(101, 10, "one.zip", 4),
                prerelease=True,
            )
        )
        with LedgerFiles() as files:
            with self.assertRaisesRegex(LEDGER.CounterError, "cannot be a prerelease"):
                ledger_inspect(files, bad_stable, names, versioned=True)
            with self.assertRaisesRegex(LEDGER.CounterError, "cannot be a prerelease"):
                ledger_prepare(
                    files, bad_stable, versioned=True, initialize=True, names=names
                )
            self.assertFalse(files.data.exists())

        legacy_nightly = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "one.zip", 4),
                prerelease=False,
            )
        )
        with LedgerFiles() as files:
            self.assertTrue(ledger_inspect(files, legacy_nightly, names).target_exists)
            with self.assertRaisesRegex(LEDGER.CounterError, "must be a prerelease"):
                ledger_prepare(files, legacy_nightly, initialize=True, names=names)

        staged_nightly = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "one.zip", 0),
                draft=True,
                prerelease=True,
            )
        )
        wrong_published = ledger_inventory(
            ledger_release(
                10,
                "nightly",
                ledger_asset(101, 10, "one.zip", 0),
                prerelease=False,
            )
        )
        with LedgerFiles() as files:
            ledger_prepare(files, ledger_inventory(), initialize=True, names=names)
            ledger_stage(files, staged_nightly, names)
            frozen = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "must be a prerelease"):
                ledger_complete(files, wrong_published, names)
            self.assertEqual(files.data.read_bytes(), frozen)
            ledger_complete(
                files,
                inventory_with_target_draft(staged_nightly, "nightly", False),
                names,
            )


class ReleaseWorkflowPreflightTests(unittest.TestCase):
    def test_active_preflight_follows_durable_snapshot_and_precedes_publication(self):
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        pending = text.index("    - name: Commit durable pending download-counter state")
        preflight = text.index(
            "    - name: Verify compatible existing release assets before publication"
        )
        bind = text.index("    - name: Bind stable release tag to workflow commit")
        freeze = text.index(
            "    - name: Reverify frozen asset counts immediately before staging"
        )
        asset_mutations = (
            "    - name: Stage new or interrupted stable versioned release as draft",
            "    - name: Stage nightly rolling release as draft",
        )

        self.assertLess(pending, preflight)
        self.assertLess(
            text.index("    - name: Verify exact source commit passed Required CI Gate"),
            preflight,
        )
        self.assertLess(preflight, bind)
        self.assertLess(bind, freeze)
        for mutation in asset_mutations:
            self.assertLess(freeze, text.index(mutation))

        next_step = text.index("\n    - name:", preflight + 1)
        step = text[preflight:next_step]
        self.assertNotIn("if: ${{ false }}", step)
        self.assertIn('prepare-download-badges.py" preflight', step)
        self.assertIn("--expected-assets-file", step)
        self.assertIn("--expected-digests-file", step)
        self.assertIn("--data-file", step)
        self.assertNotIn("git push", step)
        self.assertNotIn("gh release", step)
        self.assertIn("GITHUB_SHA: ${{ github.sha }}", step)

        freeze_next = text.index("\n    - name:", freeze + 1)
        freeze_step = text[freeze:freeze_next]
        self.assertIn('prepare-download-badges.py" preflight', freeze_step)
        self.assertIn("--expected-assets-file", freeze_step)
        self.assertIn("--expected-digests-file", freeze_step)
        self.assertIn("--data-file", freeze_step)
        self.assertEqual(
            freeze_next + 1,
            text.index(
                "    - name: Stage new or interrupted stable versioned release as draft"
            ),
        )
        self.assertEqual(text.count('prepare-download-badges.py" preflight'), 2)
        self.assertIn("group: sparkengine-publication-global", text)
        self.assertIn("cancel-in-progress: false", text)
        self.assertIn("ref: ${{ github.sha }}", text)

        uses = re.findall(r"^\s*-?\s*uses:\s*([^\s#]+)", text, flags=re.MULTILINE)
        self.assertTrue(uses)
        for action in uses:
            self.assertRegex(action.rsplit("@", 1)[1], r"^[0-9a-f]{40}$")

    def test_all_nightly_triggers_force_debug_and_release(self):
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        start = text.index("    - name: Determine build configurations")
        end = text.index("\n  # =", start)
        step = text[start:end]
        self.assertIn("IS_VERSIONED: ${{ steps.meta.outputs.is_versioned }}", step)
        self.assertIn('if [[ "$IS_VERSIONED" == "false" ]]', step)
        self.assertIn('BUILD_CONFIGS="both"', step)
        self.assertIn('configs=["Debug","Release"]', step)
        self.assertNotIn('if [[ "$EVENT_NAME" == "schedule" ]]', step)

    def test_readme_nightly_downloads_are_required_before_staging(self):
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        readme = README.read_text(encoding="utf-8")
        assets = set(
            re.findall(r"releases/download/nightly/([^\s)]+)", readme)
        )
        self.assertEqual(
            assets,
            {
                "SparkEngine-Windows-x64-Release-Installer.exe",
                "SparkEngine-Windows-x64-Release.zip",
                "SparkEngine-Windows-x64-Debug-Installer.exe",
                "SparkEngine-Windows-x64-Debug.zip",
                "SparkInstaller-Windows-x64.exe",
            },
        )
        collect = workflow.index("    - name: Collect release assets")
        stage = workflow.index("    - name: Stage nightly rolling release as draft")
        collect_step = workflow[collect:stage]
        self.assertIn("readme_nightly_assets", collect_step)
        self.assertIn('[[ ! -f "$required_asset" ]]', collect_step)
        self.assertIn("for config in Debug Release", collect_step)
        self.assertIn('"SparkEngine-Windows-x64-${config}.zip"', collect_step)
        self.assertIn(
            '"SparkEngine-Windows-x64-${config}-Installer.exe"', collect_step
        )
        self.assertIn("SparkInstaller-Windows-x64.exe", workflow)

    def test_rolling_release_uses_fail_closed_production_order(self):
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        inspect = text.index("    - name: Inspect existing release before mutation")
        mutation_gate = text.index(
            "    - name: Revalidate exact CI evidence before first release mutation"
        )
        hide = text.index("    - name: Hide and verify existing nightly release")
        prepare = text.index("    - name: Prepare durable pending download-counter state")
        pending = text.index("    - name: Commit durable pending download-counter state")
        preflight = text.index(
            "    - name: Verify compatible existing release assets before publication"
        )
        bind = text.index("    - name: Bind nightly tag to workflow commit")
        freeze = text.index(
            "    - name: Reverify frozen asset counts immediately before staging"
        )
        stage = text.index("    - name: Stage nightly rolling release as draft")
        checkpoint = text.index("    - name: Checkpoint staged release download counters")
        staged_commit = text.index("    - name: Commit staged download-counter checkpoint")
        before_tag = text.index(
            "    - name: Verify release tag immediately before publication"
        )
        publication_gate = text.index(
            "    - name: Revalidate exact CI evidence immediately before publication"
        )
        publish = text.index("    - name: Publish complete nightly rolling release")
        after_tag = text.index(
            "    - name: Verify release tag immediately after publication"
        )
        complete = text.index(
            "    - name: Complete published release and download counters"
        )
        completed_commit = text.index(
            "    - name: Commit completed download-counter state"
        )
        final_state_check = text.index(
            "    - name: Verify generated counter tag and unchanged Working"
        )

        self.assertEqual(
            [
                inspect,
                mutation_gate,
                prepare,
                pending,
                hide,
                preflight,
                freeze,
                stage,
                checkpoint,
                staged_commit,
                bind,
                before_tag,
                publication_gate,
                publish,
                after_tag,
                complete,
                completed_commit,
                final_state_check,
            ],
            sorted(
                [
                    inspect,
                    mutation_gate,
                    prepare,
                    pending,
                    hide,
                    preflight,
                    freeze,
                    stage,
                    checkpoint,
                    staged_commit,
                    bind,
                    before_tag,
                    publication_gate,
                    publish,
                    after_tag,
                    complete,
                    completed_commit,
                    final_state_check,
                ]
            ),
        )

        inspect_step = text[inspect:mutation_gate]
        self.assertIn('prepare-download-badges.py" inspect', inspect_step)
        self.assertIn("--expected-assets-file", inspect_step)
        self.assertIn("--expected-digests-file", inspect_step)

        mutation_gate_step = text[mutation_gate:prepare]
        self.assertIn("verify-exact-required-gate.py", mutation_gate_step)
        self.assertIn("TARGET_SHA: ${{ github.sha }}", mutation_gate_step)

        publication_gate_step = text[publication_gate:publish]
        self.assertIn("verify-exact-required-gate.py", publication_gate_step)
        self.assertIn("TARGET_SHA: ${{ github.sha }}", publication_gate_step)

        nightly_step = text[stage:checkpoint]
        self.assertIn("prerelease: true", nightly_step)
        nightly_publish_step = text[publish:after_tag]
        self.assertIn("-F prerelease=true", nightly_publish_step)

        hide_step = text[hide:preflight]
        self.assertIn("steps.release-inspect.outputs.target_exists == 'true'", hide_step)
        self.assertIn("steps.release-inspect.outputs.target_release_id", hide_step)
        self.assertIn("gh api --method PATCH", hide_step)
        self.assertIn("-F draft=true", hide_step)
        self.assertIn(
            'gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID"', hide_step
        )
        self.assertIn(".draft == true", hide_step)
        self.assertIn(".tag_name == $release_tag", hide_step)
        self.assertIn(".immutable == false", hide_step)
        self.assertIn("CURRENT_RELEASE", hide_step)
        self.assertLess(
            hide_step.index(
                'gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID"'
            ),
            hide_step.index("gh api --method PATCH"),
        )

        stage_step = text[stage:checkpoint]
        self.assertIn("draft: true", stage_step)
        self.assertIn("overwrite_files: true", stage_step)
        checkpoint_step = text[checkpoint:staged_commit]
        self.assertIn('prepare-download-badges.py" stage', checkpoint_step)
        self.assertIn("--expected-assets-file", checkpoint_step)
        self.assertIn("--expected-digests-file", checkpoint_step)
        staged_commit_step = text[staged_commit:before_tag]
        self.assertIn(".github/badges/downloads-data.json", staged_commit_step)
        self.assertNotIn(".github/badges/downloads.json", staged_commit_step)
        self.assertNotIn(".github/badges/installer-downloads.json", staged_commit_step)
        self.assertIn("staged_commit=$STAGED_COMMIT", staged_commit_step)
        before_tag_step = text[before_tag:publish]
        self.assertIn("fetch --force --no-tags", before_tag_step)
        self.assertIn('refs/tags/${RELEASE_TAG}^{commit}', before_tag_step)
        publish_step = text[publish:after_tag]
        self.assertIn("-F draft=false", publish_step)
        after_tag_step = text[after_tag:complete]
        self.assertIn("fetch --force --no-tags", after_tag_step)
        self.assertIn('refs/tags/${RELEASE_TAG}^{commit}', after_tag_step)
        complete_step = text[complete:completed_commit]
        self.assertIn('prepare-download-badges.py" complete', complete_step)
        self.assertIn("--expected-assets-file", complete_step)
        self.assertIn("--expected-digests-file", complete_step)
        completed_commit_step = text[completed_commit:final_state_check]
        self.assertIn(
            'EXPECTED_STAGED="${{ steps.badge-staged-commit.outputs.staged_commit }}"',
            completed_commit_step,
        )
        self.assertIn(".github/badges/downloads-data.json", completed_commit_step)
        self.assertIn(".github/badges/downloads.json", completed_commit_step)
        self.assertIn(".github/badges/installer-downloads.json", completed_commit_step)
        self.assertIn("STATE_REF: refs/tags/generated-release-counters", completed_commit_step)
        self.assertIn('"HEAD:${STATE_REF}"', completed_commit_step)
        self.assertNotIn("refs/heads/Working", completed_commit_step)
        final_state_step = text[final_state_check:]
        self.assertIn("TRUSTED_WORKING_SHA", final_state_step)
        self.assertNotIn("gh workflow run build.yml", final_state_step)
        self.assertNotIn('prepare-download-badges.py" finalize', text)

    def test_existing_versioned_release_is_immutable_or_resumed_without_overwrite(self):
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        stage = text.index(
            "    - name: Stage new or interrupted stable versioned release as draft"
        )
        nightly = text.index("    - name: Stage nightly rolling release as draft")
        step = text[stage:nightly]
        self.assertIn("steps.release-freeze.outputs.target_exists != 'true'", step)
        self.assertIn("steps.release-freeze.outputs.target_is_draft == 'true'", step)
        self.assertIn("overwrite_files: false", step)
        self.assertIn("draft: true", step)
        publish = text.index("    - name: Publish complete stable versioned release")
        nightly_publish = text.index("    - name: Publish complete nightly rolling release")
        publish_step = text[publish:nightly_publish]
        self.assertIn("target_is_draft == 'true'", publish_step)
        self.assertIn("target_release_id", publish_step)

    def test_exact_ci_evidence_asset_is_recoverable_and_terminally_revalidated(self):
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        required = text.index(
            "    - name: Verify exact source commit passed Required CI Gate"
        )
        readiness = text.index(
            "    - name: Verify stable-v1 is ready for versioned publication"
        )
        freeze = text.index("    - name: Freeze durable exact CI evidence release asset")
        mutation_manifest = text.index(
            "    - name: Verify frozen exact CI evidence before first release mutation"
        )
        mutation_after = text.index(
            "    - name: Hide and verify existing nightly release",
            mutation_manifest,
        )
        collect = text.index("    - name: Collect release assets")
        inspect = text.index("    - name: Inspect existing release before mutation")
        prepare = text.index("    - name: Prepare durable pending download-counter state")
        pending = text.index("    - name: Commit durable pending download-counter state")
        cleanup_preflight = text.index(
            "    - name: Preflight durable stable-draft failed-upload cleanup"
        )
        cleanup_delete = text.index(
            "    - name: Delete only durably authorized failed stable-draft uploads"
        )
        cleanup_accept = text.index(
            "    - name: Accept stable-draft cleanup into the durable pending ledger"
        )
        cleanup_commit = text.index(
            "    - name: Commit accepted stable-draft cleanup state"
        )
        replacement_preflight = text.index(
            "    - name: Preflight durable exact CI evidence replacement"
        )
        delete = text.index(
            "    - name: Delete only the durably authorized stable-draft evidence asset"
        )
        upload = text.index(
            "    - name: Upload only the replacement stable-draft evidence asset"
        )
        accept = text.index(
            "    - name: Accept replacement evidence asset into the durable pending ledger"
        )
        accepted_commit = text.index(
            "    - name: Commit accepted exact CI evidence replacement state"
        )
        ordinary_preflight = text.index(
            "    - name: Verify compatible existing release assets before publication"
        )
        final_gate = text.index(
            "    - name: Revalidate exact CI evidence immediately before publication"
        )
        final_manifest = text.index(
            "    - name: Verify frozen exact CI evidence manifest immediately before publication"
        )
        final_full = text.index(
            "    - name: Reverify complete staged release against durable checkpoint before publication"
        )
        final_asset = text.index(
            "    - name: Verify complete staged release immediately before publication"
        )
        stable_publish = text.index(
            "    - name: Publish complete stable versioned release"
        )
        nightly_publish = text.index(
            "    - name: Publish complete nightly rolling release"
        )

        ordered = (
            required,
            readiness,
            freeze,
            collect,
            inspect,
            prepare,
            pending,
            cleanup_preflight,
            cleanup_delete,
            cleanup_accept,
            cleanup_commit,
            replacement_preflight,
            delete,
            upload,
            accept,
            accepted_commit,
            ordinary_preflight,
            final_gate,
            final_manifest,
            final_full,
            final_asset,
            stable_publish,
            nightly_publish,
        )
        self.assertEqual(list(ordered), sorted(ordered))

        freeze_step = text[freeze:collect]
        self.assertIn("steps.required-ci.outputs.run_id", freeze_step)
        self.assertIn("steps.required-ci.outputs.build_matrix_status_id", freeze_step)
        self.assertIn("steps.required-ci.outputs.codeql_status_id", freeze_step)
        self.assertIn("tools/site-data/exact_evidence.py write", freeze_step)
        self.assertNotIn("steps.required-ci-final.outputs", freeze_step)

        mutation_manifest_step = text[mutation_manifest:mutation_after]
        final_manifest_step = text[final_manifest:final_full]
        for block, output_step in (
            (freeze_step, "required-ci"),
            (mutation_manifest_step, "required-ci-mutation"),
            (final_manifest_step, "required-ci-final"),
        ):
            with self.subTest(output_step=output_step):
                actual_pairs = re.findall(
                    r"^        (EXACT_[A-Z0-9_]+): (.+)$",
                    block,
                    flags=re.MULTILINE,
                )
                expected_pairs = [("EXACT_SOURCE_COMMIT", "${{ github.sha }}")]
                expected_pairs.extend(
                    (
                        environment_name,
                        f"${{{{ steps.{output_step}.outputs.{output_name} }}}}",
                    )
                    for output_name, environment_name in EXACT_EVIDENCE.ENV_FROM_GATE_KEY.items()
                )
                self.assertEqual(len(actual_pairs), 56)
                self.assertCountEqual(actual_pairs, expected_pairs)

        collect_step = text[collect:inspect]
        evidence = "SparkEngine-Exact-CI-Evidence.json"
        self.assertIn(f'-name "{evidence}"', collect_step)
        self.assertIn(f'if [[ "$artifact" != "{evidence}" ]]', collect_step)
        self.assertIn('sha256sum "$artifact" >> SHA256SUMS', collect_step)
        self.assertIn('printf \'%s\' "$FILES" > expected-release-assets.txt', collect_step)
        self.assertIn(
            'done < expected-release-assets.txt > expected-release-digests.txt',
            collect_step,
        )
        self.assertIn(
            "The exact-CI manifest is separately bound to its GitHub asset digest",
            collect_step,
        )

        inspect_step = text[inspect:prepare]
        prepare_step = text[prepare:pending]
        self.assertIn("--allow-exact-ci-evidence-replacement", inspect_step)
        self.assertIn("--allow-stable-draft-recovery-candidates", inspect_step)
        self.assertIn("--allow-exact-ci-evidence-replacement", prepare_step)
        self.assertEqual(text.count("--allow-exact-ci-evidence-replacement"), 2)
        self.assertEqual(text.count("--allow-stable-draft-recovery-candidates"), 1)

        cleanup_delete_step = text[cleanup_delete:cleanup_accept]
        self.assertIn("steps.draft-cleanup.outputs.cleanup_assets", cleanup_delete_step)
        self.assertIn("jq -c '.[]'", cleanup_delete_step)
        self.assertIn(".download_count == $download_count", cleanup_delete_step)
        self.assertIn('if $asset_digest == "" then .digest == null', cleanup_delete_step)
        self.assertIn(".draft == true", cleanup_delete_step)
        self.assertIn(".prerelease == false", cleanup_delete_step)
        self.assertIn("gh api --method DELETE", cleanup_delete_step)
        self.assertIn(
            '"repos/$GITHUB_REPOSITORY/releases/assets/$ASSET_ID"',
            cleanup_delete_step,
        )

        delete_step = text[delete:upload]
        self.assertIn("delete_evidence_asset_digest", delete_step)
        self.assertIn("delete_evidence_asset_state", delete_step)
        self.assertIn("delete_evidence_download_count", delete_step)
        self.assertIn(
            'gh api "repos/$GITHUB_REPOSITORY/releases/assets/$DELETE_EVIDENCE_ASSET_ID"',
            delete_step,
        )
        self.assertIn(".download_count == $download_count", delete_step)
        self.assertIn('if $asset_digest == "" then .digest == null', delete_step)
        self.assertIn(".state == $asset_state", delete_step)
        self.assertIn(".draft == true", delete_step)
        self.assertIn(".prerelease == false", delete_step)
        self.assertIn(
            'gh api --method DELETE \\\n          "repos/$GITHUB_REPOSITORY/releases/assets/$DELETE_EVIDENCE_ASSET_ID"',
            delete_step,
        )
        self.assertNotIn("gh release delete-asset", delete_step)
        self.assertNotIn("--cleanup-tag", delete_step)

        upload_step = text[upload:accept]
        self.assertIn(
            "steps.evidence-replacement.outputs.target_release_id",
            upload_step,
        )
        self.assertIn(
            'gh api "repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID"',
            upload_step,
        )
        self.assertIn(".draft == true", upload_step)
        self.assertIn(".prerelease == false", upload_step)
        self.assertIn(
            '"https://uploads.github.com/repos/$GITHUB_REPOSITORY/releases/$RELEASE_ID/assets?name=SparkEngine-Exact-CI-Evidence.json"',
            upload_step,
        )
        self.assertIn(".upload_url == $upload_template", upload_step)
        self.assertIn("gh api --method POST", upload_step)
        self.assertNotIn("--hostname uploads.github.com", upload_step)
        self.assertIn('--input "$EVIDENCE_PATH"', upload_step)
        self.assertIn(".digest == $asset_digest", upload_step)
        self.assertNotIn('gh release upload "$RELEASE_TAG"', upload_step)
        self.assertNotIn("--clobber", upload_step)
        self.assertNotIn("SHA256SUMS", upload_step)

        accepted_step = text[accepted_commit:ordinary_preflight]
        self.assertIn(
            "EXPECTED_PENDING: ${{ steps.badge-cleanup-commit.outputs.pending_commit || steps.badge-pending-commit.outputs.pending_commit }}",
            accepted_step,
        )
        checkpoint = text.index("    - name: Commit staged download-counter checkpoint")
        checkpoint_step = text[checkpoint:final_gate]
        self.assertIn(
            "steps.badge-replacement-commit.outputs.pending_commit || steps.badge-cleanup-commit.outputs.pending_commit || steps.badge-pending-commit.outputs.pending_commit",
            checkpoint_step,
        )

        self.assertIn("steps.required-ci-final.outputs.run_id", final_manifest_step)
        self.assertIn("steps.required-ci-final.outputs.build_matrix_status_id", final_manifest_step)
        self.assertIn("steps.required-ci-final.outputs.codeql_status_id", final_manifest_step)
        self.assertIn("exact_evidence.py verify", final_manifest_step)
        final_full_step = text[final_full:final_asset]
        self.assertIn('prepare-download-badges.py" publish-preflight', final_full_step)
        self.assertIn("--expected-assets-file", final_full_step)
        self.assertIn("--expected-digests-file", final_full_step)
        self.assertIn("badge-repository/.github/badges/downloads-data.json", final_full_step)
        final_asset_step = text[final_asset:stable_publish]
        self.assertIn("verify_release_asset_boundary.py", final_asset_step)
        self.assertIn('--release-json "$RELEASE_RECORD"', final_asset_step)
        self.assertIn('--assets-json "$RELEASE_ASSETS"', final_asset_step)
        self.assertIn("--ledger-json", final_asset_step)
        self.assertIn("--expected-draft true", final_asset_step)

        asset_boundary = ASSET_BOUNDARY_SCRIPT.read_text(encoding="utf-8")
        for required in (
            "preparedAssetIds",
            "draftCleanup",
            "actual_by_name",
            "EXPECTED_UPLOADER_ID = 41898282",
            'EXPECTED_UPLOADER_LOGIN = "github-actions[bot]"',
            'asset.get("size")',
            'asset.get("digest")',
            'release.get("immutable") is False',
            "live_count >= ledger_count",
        ):
            self.assertIn(required, asset_boundary)

        stable_publish_step = text[stable_publish:nightly_publish]
        self.assertIn("-F prerelease=false", stable_publish_step)
        self.assertIn(".prerelease == false", stable_publish_step)
        nightly_after = text.index(
            "    - name: Verify release tag immediately after publication"
        )
        nightly_publish_step = text[nightly_publish:nightly_after]
        recovery_script = PUBLICATION_RECOVERY_SCRIPT.read_text(encoding="utf-8")
        for publish_step in (stable_publish_step, nightly_publish_step):
            exact_gate = publish_step.index("verify-exact-required-gate.py")
            frozen_manifest = publish_step.index("exact_evidence.py")
            tag_recheck = publish_step.index('refs/tags/${RELEASE_TAG}^{commit}')
            staged_release_recheck = publish_step.index("publish-preflight")
            counter_state_recheck = publish_step.index("REMOTE_STAGED_COMMIT")
            working_recheck = publish_step.index("CURRENT_WORKING_SHA")
            final_exact_replay = publish_step.rindex("verify-exact-required-gate.py")
            final_manifest_replay = publish_step.rindex("exact_evidence.py")
            boundary_calls = [
                match.start()
                for match in re.finditer(
                    "verify-release-publication-boundary.sh", publish_step
                )
            ]
            self.assertEqual(len(boundary_calls), 2)
            terminal_boundary, post_publish_boundary = boundary_calls
            visibility_mutation = publish_step.index("\n        gh api --method PATCH")
            trap_clear = publish_step.rindex("trap - ERR")
            self.assertLess(exact_gate, frozen_manifest)
            self.assertLess(frozen_manifest, tag_recheck)
            self.assertLess(tag_recheck, staged_release_recheck)
            self.assertLess(staged_release_recheck, counter_state_recheck)
            self.assertLess(counter_state_recheck, working_recheck)
            self.assertLess(working_recheck, final_exact_replay)
            self.assertLess(final_exact_replay, final_manifest_replay)
            self.assertLess(final_manifest_replay, terminal_boundary)
            self.assertLess(terminal_boundary, visibility_mutation)
            self.assertLess(visibility_mutation, post_publish_boundary)
            self.assertLess(post_publish_boundary, trap_clear)
            attempted = publish_step.index("publication_attempted=true")
            recovered = publish_step.index("publication_attempted=false", attempted)
            self.assertLess(terminal_boundary, attempted)
            self.assertLess(attempted, visibility_mutation)
            self.assertLess(post_publish_boundary, recovered)
            self.assertLess(recovered, trap_clear)
            self.assertEqual(publish_step.count("verify-exact-required-gate.py"), 2)
            self.assertEqual(publish_step.count("verify-gate-output"), 2)
            self.assertEqual(
                publish_step.count('prepare-download-badges.py" publish-preflight'), 1
            )
            self.assertEqual(publish_step.count("\n          publish-preflight"), 1)
            self.assertEqual(publish_step.count("\n          post-publish-preflight"), 1)
            self.assertIn("GITHUB_OUTPUT=\"$FINAL_GATE_OUTPUT\"", publish_step)
            self.assertIn("verify-gate-output", publish_step)
            self.assertIn("--manifest", publish_step)
            self.assertIn("fetch --force --no-tags", publish_step)
            self.assertIn("--expected-assets-file", publish_step)
            self.assertIn("--expected-digests-file", publish_step)
            self.assertIn("badge-repository/.github/badges/downloads-data.json", publish_step)
            self.assertIn("steps.badge-staged-commit.outputs.staged_commit", publish_step)
            self.assertIn("refs/tags/generated-release-counters", publish_step)
            self.assertIn('commits/Working" --jq', publish_step)
            self.assertIn("redraft_failed_publication", publish_step)
            self.assertIn("recover_release_publication.py", publish_step)
            self.assertIn("--attempts 3", publish_step)
            self.assertNotIn("continue-on-error", publish_step)
            self.assertNotIn("|| true", publish_step)

        self.assertIn("--expected-prerelease false", stable_publish_step)
        self.assertIn("--expected-prerelease true", nightly_publish_step)
        for required in (
            "api.get_release(release_id)",
            "api.hide_release(release_id)",
            'if current["draft"]',
            'if current["immutable"]',
            'if recovered["draft"] and not recovered["immutable"]',
            '"PATCH", release_id, {"draft": True, "make_latest": "false"}',
        ):
            self.assertIn(required, recovery_script)
        self.assertLess(
            recovery_script.index("api.get_release(release_id)"),
            recovery_script.index("api.hide_release(release_id)"),
        )
        self.assertGreaterEqual(recovery_script.count("api.get_release(release_id)"), 2)

        boundary_script = PUBLICATION_BOUNDARY_SCRIPT.read_text(encoding="utf-8")
        for required in (
            'fetch --force --no-tags "$repository_url"',
            'refs/tags/${RELEASE_TAG}^{commit}',
            'ls-remote --refs "$repository_url" "$STATE_REF"',
            'commits/Working" --jq',
            'prepare-download-badges.py" "$phase"',
            'boundary_release_id" != "$RELEASE_ID"',
            'boundary_draft" != "$expected_draft"',
            "immutable-releases",
            ".enabled == false",
            "releases/$RELEASE_ID/assets?per_page=100",
            "verify_release_asset_boundary.py",
            '--expected-draft "$expected_draft"',
        ):
            self.assertIn(required, boundary_script)
        self.assertLess(
            boundary_script.index('commits/Working" --jq'),
            boundary_script.index('prepare-download-badges.py" "$phase"'),
        )

        stable_stage = text.index(
            "    - name: Stage new or interrupted stable versioned release as draft"
        )
        nightly_stage = text.index("    - name: Stage nightly rolling release as draft")
        self.assertIn("overwrite_files: false", text[stable_stage:nightly_stage])
        for step in (
            text[replacement_preflight:delete],
            delete_step,
            upload_step,
            text[accept:accepted_commit],
            final_manifest_step,
            final_full_step,
            final_asset_step,
        ):
            self.assertNotIn("continue-on-error", step)
            self.assertNotIn("|| true", step)

    def test_six_hour_refresh_persists_durable_ledger_badges(self):
        text = UPDATE_WORKFLOW.read_text(encoding="utf-8")
        refresh = text.index('prepare-download-badges.py" refresh')
        add = text.index("        git add --", refresh)
        commit = text.index("        git commit -s -m", add)
        self.assertLess(refresh, add)
        staged = text[add:commit]
        self.assertIn(".github/badges/downloads-data.json", staged)
        self.assertIn(".github/badges/downloads.json", staged)
        self.assertIn(".github/badges/installer-downloads.json", staged)
        self.assertIn("group: sparkengine-publication-global", text)
        self.assertIn("cancel-in-progress: false", text)
        self.assertIn("STATE_REF: refs/tags/generated-release-counters", text)
        self.assertIn('"HEAD:${STATE_REF}"', text)
        self.assertNotIn('"HEAD:refs/heads/Working"', text)
        self.assertNotIn("gh workflow run build.yml", text)
        readme = README.read_text(encoding="utf-8")
        self.assertIn("generated-release-counters%2F.github%2Fbadges%2Fdownloads.json", readme)
        self.assertIn(
            "generated-release-counters%2F.github%2Fbadges%2Finstaller-downloads.json",
            readme,
        )


class V2FakeApi:
    def __init__(self, responses):
        self.responses = {key: list(values) for key, values in responses.items()}
        self.calls = []

    def get(self, path_or_url):
        self.calls.append(path_or_url)
        key = path_or_url.removeprefix("https://api.github.com")
        if key not in self.responses or not self.responses[key]:
            raise AssertionError(f"unexpected API request: {key}")
        value = self.responses[key].pop(0)
        if isinstance(value, Exception):
            raise value
        return value


class V2ApiTests(unittest.TestCase):
    def test_inventory_requires_release_state_and_valid_sha256_digests(self):
        release_page = f"/repos/{REPOSITORY}/releases?per_page=100&page=1"
        asset_page = f"/repos/{REPOSITORY}/releases/1/assets?per_page=100&page=1"

        missing_draft = V2FakeApi(
            {
                release_page: [
                    LEDGER.ApiResponse([{"id": 1, "tag_name": "nightly"}], None)
                ]
            }
        )
        with self.assertRaisesRegex(LEDGER.CounterError, "draft must be boolean"):
            LEDGER.fetch_inventory_once(missing_draft, REPOSITORY)

        missing_prerelease = V2FakeApi(
            {
                release_page: [
                    LEDGER.ApiResponse(
                        [{"id": 1, "tag_name": "nightly", "draft": False}], None
                    )
                ]
            }
        )
        with self.assertRaisesRegex(LEDGER.CounterError, "prerelease must be boolean"):
            LEDGER.fetch_inventory_once(missing_prerelease, REPOSITORY)

        invalid_digest = V2FakeApi(
            {
                release_page: [
                    LEDGER.ApiResponse(
                        [
                            {
                                "id": 1,
                                "tag_name": "nightly",
                                "draft": False,
                                "prerelease": True,
                            }
                        ],
                        None,
                    )
                ],
                asset_page: [
                    LEDGER.ApiResponse(
                        [
                            {
                                "id": 7,
                                "name": "one.zip",
                                "download_count": 0,
                                "digest": "md5:bad",
                            }
                        ],
                        None,
                    )
                ],
            }
        )
        with self.assertRaisesRegex(LEDGER.CounterError, "SHA-256 digest"):
            LEDGER.fetch_inventory_once(invalid_digest, REPOSITORY)

        invalid_state = V2FakeApi(
            {
                release_page: [
                    LEDGER.ApiResponse(
                        [
                            {
                                "id": 1,
                                "tag_name": "nightly",
                                "draft": False,
                                "prerelease": True,
                            }
                        ],
                        None,
                    )
                ],
                asset_page: [
                    LEDGER.ApiResponse(
                        [
                            {
                                "id": 7,
                                "name": "one.zip",
                                "download_count": 0,
                                "digest": asset_digest("one.zip"),
                                "state": "ghost",
                            }
                        ],
                        None,
                    )
                ],
            }
        )
        with self.assertRaisesRegex(LEDGER.CounterError, "starter or uploaded"):
            LEDGER.fetch_inventory_once(invalid_state, REPOSITORY)

    def test_terminal_full_page_is_probed_before_completion(self):
        page1 = f"/repos/{REPOSITORY}/releases?per_page=100&page=1"
        page2 = f"/repos/{REPOSITORY}/releases?per_page=100&page=2"
        full = [{"id": index, "tag_name": f"v0.0.{index}"} for index in range(1, 101)]
        api = V2FakeApi(
            {
                page1: [LEDGER.ApiResponse(full, None)],
                page2: [LEDGER.ApiResponse([], None)],
            }
        )
        self.assertEqual(len(LEDGER.fetch_all_releases(api, REPOSITORY)), 100)
        self.assertEqual(api.calls, [page1, page2])

    def test_pagination_cycle_nonprogress_and_untrusted_url_fail_closed(self):
        page1 = f"/repos/{REPOSITORY}/releases?per_page=100&page=1"
        full = [{"id": index, "tag_name": f"v0.0.{index}"} for index in range(1, 101)]
        bad_links = (
            (
                f"https://api.github.com/repos/{REPOSITORY}/releases?per_page=100&page=1",
                "did not advance",
            ),
            ("https://example.invalid/steal?page=2", "unexpected GitHub API URL"),
        )
        for link, message in bad_links:
            with self.subTest(link=link):
                api = V2FakeApi({page1: [LEDGER.ApiResponse(full, link)]})
                with self.assertRaisesRegex(LEDGER.CounterError, message):
                    LEDGER.fetch_all_releases(api, REPOSITORY)
                self.assertEqual(api.calls, [page1])

    def test_duplicates_moving_inventory_and_api_failure_fail_closed(self):
        page1 = f"/repos/{REPOSITORY}/releases?per_page=100&page=1"
        duplicate_api = V2FakeApi(
            {
                page1: [
                    LEDGER.ApiResponse(
                        [{"id": 1, "tag_name": "v1"}, {"id": 1, "tag_name": "v2"}],
                        None,
                    )
                ]
            }
        )
        with self.assertRaisesRegex(LEDGER.CounterError, "duplicate ID"):
            LEDGER.fetch_all_releases(duplicate_api, REPOSITORY)

        asset_page = f"/repos/{REPOSITORY}/releases/1/assets?per_page=100&page=1"
        duplicate_asset_api = V2FakeApi(
            {
                asset_page: [
                    LEDGER.ApiResponse(
                        [
                            {"id": 7, "name": "a", "download_count": 0},
                            {"id": 7, "name": "b", "download_count": 0},
                        ],
                        None,
                    )
                ]
            }
        )
        with self.assertRaisesRegex(LEDGER.CounterError, "duplicate asset ID"):
            LEDGER.fetch_release_assets(duplicate_asset_api, REPOSITORY, 1)

        first = ledger_inventory(
            ledger_release(1, "nightly", ledger_asset(7, 1, "a.zip", 5))
        )
        for second, message in (
            (
                ledger_inventory(
                    ledger_release(1, "nightly", ledger_asset(8, 1, "a.zip", 5))
                ),
                "changed",
            ),
            (
                ledger_inventory(
                    ledger_release(1, "nightly", ledger_asset(7, 1, "a.zip", 4))
                ),
                "regressed",
            ),
            (
                ledger_inventory(
                    ledger_release(
                        1,
                        "nightly",
                        ledger_asset(7, 1, "a.zip", 5),
                        prerelease=False,
                    )
                ),
                "changed",
            ),
        ):
            with self.subTest(message=message), mock.patch.object(
                LEDGER, "fetch_inventory_once", side_effect=(first, second)
            ):
                with self.assertRaisesRegex(LEDGER.CounterError, message):
                    LEDGER.fetch_consistent_inventory(object(), REPOSITORY)

        failed_api = V2FakeApi({page1: [LEDGER.CounterError("rate limited")]})
        with self.assertRaisesRegex(LEDGER.CounterError, "rate limited"):
            LEDGER.fetch_all_releases(failed_api, REPOSITORY)


if __name__ == "__main__":
    unittest.main(verbosity=2)
