#!/usr/bin/env python3
"""Security and recovery tests for the schema-v2 download counter ledger."""

from __future__ import annotations

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
SPEC = importlib.util.spec_from_file_location("download_counter_ledger", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
LEDGER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LEDGER
SPEC.loader.exec_module(LEDGER)

REPOSITORY = "Krilliac/SparkEngine"
SOURCE_SHA = "a" * 40
FIXED_NOW = lambda: datetime(2026, 8, 27, tzinfo=timezone.utc)


def ledger_asset(asset_id: int, release_id: int, name: str, count: int):
    return LEDGER.AssetRecord(asset_id, release_id, name, count)


def ledger_release(release_id: int, tag: str, *assets):
    return LEDGER.ReleaseRecord(release_id, tag, tuple(assets))


def ledger_inventory(*releases):
    return LEDGER.Inventory(tuple(releases))


class LedgerFiles:
    def __enter__(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.badges = self.root / "badges"
        self.data = self.badges / "downloads-data.json"
        self.expected = self.root / "expected-assets.txt"
        return self

    def __exit__(self, *args):
        self.temporary.cleanup()


def ledger_prepare(files, current, *, versioned=False, attempt="1", initialize=False):
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.prepare_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=SOURCE_SHA,
            target_tag="v1.2.3" if versioned else "nightly",
            data_file=files.data,
            badge_directory=files.badges,
            api=object(),
            initialize=initialize,
            now=FIXED_NOW,
        )


def ledger_finalize(files, current, names, *, versioned=False, attempt="1"):
    files.expected.write_text("".join(f"{name}\n" for name in names), encoding="utf-8")
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.finalize_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=SOURCE_SHA,
            target_tag="v1.2.3" if versioned else "nightly",
            expected_assets_file=files.expected,
            data_file=files.data,
            badge_directory=files.badges,
            api=object(),
            now=FIXED_NOW,
        )


def ledger_preflight(files, current, names, *, versioned=False, attempt="1"):
    files.expected.write_text("".join(f"{name}\n" for name in names), encoding="utf-8")
    with mock.patch.object(LEDGER, "fetch_consistent_inventory", return_value=current):
        return LEDGER.preflight_publication(
            repository=REPOSITORY,
            is_versioned=versioned,
            run_id="700",
            run_attempt=attempt,
            source_sha=SOURCE_SHA,
            target_tag="v1.2.3" if versioned else "nightly",
            expected_assets_file=files.expected,
            data_file=files.data,
            api=object(),
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
            final = ledger_finalize(
                files, current, ("engine.zip", "Engine-Setup.MSI"), versioned=True
            )
            self.assertEqual((prepared.total, prepared.installer_total), (12, 5))
            self.assertEqual((final.total, final.installer_total), (12, 5))

    def test_stable_and_nightly_replacement_keep_old_high_water_once(self):
        for versioned in (False, True):
            with self.subTest(versioned=versioned), LedgerFiles() as files:
                tag = "v1.2.3" if versioned else "nightly"
                old = ledger_inventory(
                    ledger_release(10, tag, ledger_asset(101, 10, "old.zip", 9))
                )
                new = ledger_inventory(
                    ledger_release(10, tag, ledger_asset(202, 10, "new.zip", 1))
                )
                ledger_prepare(files, old, versioned=versioned, initialize=True)
                final = ledger_finalize(files, new, ("new.zip",), versioned=versioned)
                state = LEDGER.load_state(files.data)
                badge_name = (
                    "stable-downloads.json" if versioned else "nightly-downloads.json"
                )
                badge = json.loads((files.badges / badge_name).read_text())
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
            self.assertEqual(
                ledger_finalize(files, new, ("new.zip",), attempt="2").total, 8
            )

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

    def test_preflight_allows_first_publication_and_exact_stable_or_nightly_rerun(self):
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
                    self.assertEqual((result.target_exists, result.target_asset_count), (exists, count))
                    self.assertEqual(files.data.read_bytes(), before)

    def test_preflight_rejects_exact_set_mismatch_without_changing_pending_state(self):
        for versioned in (False, True):
            tag = "v1.2.3" if versioned else "nightly"
            cases = (
                (
                    ledger_inventory(
                        ledger_release(10, tag, ledger_asset(101, 10, "one.zip", 4))
                    ),
                    ("one.zip", "two.zip"),
                    "missing: two.zip",
                ),
                (
                    ledger_inventory(
                        ledger_release(
                            10,
                            tag,
                            ledger_asset(101, 10, "one.zip", 4),
                            ledger_asset(102, 10, "obsolete.zip", 7),
                        )
                    ),
                    ("one.zip",),
                    "unexpected: obsolete.zip",
                ),
            )
            for current, expected, message in cases:
                with self.subTest(versioned=versioned, message=message), LedgerFiles() as files:
                    ledger_prepare(files, current, versioned=versioned, initialize=True)
                    before = files.data.read_bytes()
                    with self.assertRaisesRegex(LEDGER.CounterError, message):
                        ledger_preflight(files, current, expected, versioned=versioned)
                    self.assertEqual(files.data.read_bytes(), before)
                    self.assertIsNotNone(LEDGER.load_state(files.data).pending)

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

    def test_preflight_requires_the_exact_pending_operation(self):
        current = ledger_inventory(
            ledger_release(10, "nightly", ledger_asset(101, 10, "one.zip", 4))
        )
        with LedgerFiles() as files:
            ledger_prepare(files, current, initialize=True)
            before = files.data.read_bytes()
            with self.assertRaisesRegex(LEDGER.CounterError, "does not match"):
                ledger_preflight(files, current, ("one.zip",), attempt="2")
            self.assertEqual(files.data.read_bytes(), before)


class ReleaseWorkflowPreflightTests(unittest.TestCase):
    def test_active_preflight_follows_durable_snapshot_and_precedes_publication(self):
        text = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        pending = text.index("    - name: Commit durable pending download-counter state")
        preflight = text.index(
            "    - name: Verify exact existing release asset set before publication"
        )
        mutations = (
            "    - name: Bind release tag to workflow commit",
            "    - name: Create stable versioned release",
            "    - name: Create or update nightly rolling release",
        )

        self.assertLess(pending, preflight)
        self.assertLess(
            text.index("    - name: Verify exact source commit passed Required CI Gate"),
            preflight,
        )
        for mutation in mutations:
            self.assertLess(preflight, text.index(mutation))

        next_step = text.index("\n    - name:", preflight + 1)
        step = text[preflight:next_step]
        self.assertNotIn("if: ${{ false }}", step)
        self.assertIn('prepare-download-badges.py" preflight', step)
        self.assertIn("--expected-assets-file", step)
        self.assertIn("--data-file", step)
        self.assertNotIn("git push", step)
        self.assertNotIn("gh release", step)
        self.assertIn("GITHUB_SHA: ${{ github.sha }}", step)
        self.assertIn("group: sparkengine-publication-global", text)
        self.assertIn("cancel-in-progress: false", text)
        self.assertIn("ref: ${{ github.sha }}", text)

        uses = re.findall(r"^\s*-?\s*uses:\s*([^\s#]+)", text, flags=re.MULTILINE)
        self.assertTrue(uses)
        for action in uses:
            self.assertRegex(action.rsplit("@", 1)[1], r"^[0-9a-f]{40}$")


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
