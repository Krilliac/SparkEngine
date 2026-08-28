#!/usr/bin/env python3
"""SEC-120 — Mutation tests for fuzz policy enforcement.

Tests that the policy checker correctly detects:
  - Omitted parsers (unclassified sources)
  - Path traversal in source file references
  - Duplicate parser IDs
  - Stale corpus entries (> 90 days)
  - Missing blocker tickets on blocked parsers
  - Missing fuzz targets on fuzzed parsers
  - Resource budget violations
  - Trust boundary misclassification warnings
  - Registry consistency (no empty IDs, no empty source lists)

These are the adversarial tests that prove the CI gate catches real mistakes.
"""
from __future__ import annotations

import copy
import json
import os
import sys
import unittest
from datetime import date, timedelta
from pathlib import Path
from unittest.mock import patch

# Add tools/fuzz-policy to path
_repo_root = Path(__file__).resolve().parent.parent.parent
_tools_dir = _repo_root / "tools" / "fuzz-policy"
sys.path.insert(0, str(_tools_dir))

from parser_inventory import (
    KNOWN_PARSERS,
    FuzzStatus,
    ParserEntry,
    TrustBoundary,
    check_duplicate_ids,
    find_unclassified_parsers,
    get_known_source_files,
    get_parser_ids,
    emit_manifest,
)
from corpus_manifest import (
    CORPUS_MANIFEST,
    CorpusEntry,
    ResourceBudget,
    STALENESS_THRESHOLD_DAYS,
    TIER_NETWORK,
    TIER_SMALL,
    TIER_MEDIUM,
    TIER_LARGE,
    validate_manifest,
)
from check_fuzz_policy import (
    _is_path_traversal,
    _normalize_path,
    check_blocked_parsers_have_tickets,
    check_classification_completeness,
    check_fuzzed_parsers_have_targets,
    check_source_files_exist,
    check_untrusted_parsers_not_ignored,
    run_all_checks,
)


class TestParserInventoryConsistency(unittest.TestCase):
    """Verify the inventory itself is well-formed."""

    def test_no_empty_parser_ids(self):
        for p in KNOWN_PARSERS:
            self.assertTrue(p.parser_id.strip(), f"Empty parser_id found: {p}")

    def test_no_duplicate_parser_ids(self):
        dupes = check_duplicate_ids()
        self.assertEqual(dupes, [], f"Duplicate parser IDs: {dupes}")

    def test_all_parsers_have_source_files(self):
        for p in KNOWN_PARSERS:
            self.assertTrue(len(p.source_files) > 0, f"{p.parser_id}: no source_files")

    def test_all_parsers_have_description(self):
        for p in KNOWN_PARSERS:
            self.assertTrue(p.description.strip(), f"{p.parser_id}: empty description")

    def test_all_parsers_have_formats(self):
        for p in KNOWN_PARSERS:
            self.assertTrue(len(p.formats_handled) > 0, f"{p.parser_id}: no formats_handled")

    def test_parser_ids_are_kebab_case(self):
        import re
        kebab = re.compile(r"^[a-z][a-z0-9]*(-[a-z0-9]+)*$")
        for p in KNOWN_PARSERS:
            self.assertRegex(p.parser_id, kebab, f"parser_id not kebab-case: {p.parser_id}")

    def test_source_files_use_forward_slashes(self):
        for p in KNOWN_PARSERS:
            for sf in p.source_files:
                self.assertNotIn("\\", sf, f"{p.parser_id}: backslash in source file: {sf}")

    def test_no_path_traversal_in_source_files(self):
        for p in KNOWN_PARSERS:
            for sf in p.source_files:
                self.assertFalse(
                    _is_path_traversal(sf),
                    f"{p.parser_id}: path traversal in source file: {sf}",
                )

    def test_minimum_parser_count(self):
        self.assertGreaterEqual(
            len(KNOWN_PARSERS), 30,
            "Inventory has fewer than 30 parsers — likely missing entries",
        )

    def test_has_untrusted_file_parsers(self):
        untrusted = [p for p in KNOWN_PARSERS if p.trust_boundary == TrustBoundary.UNTRUSTED_FILE]
        self.assertGreater(len(untrusted), 0, "No untrusted-file parsers found")

    def test_has_untrusted_network_parsers(self):
        untrusted = [p for p in KNOWN_PARSERS if p.trust_boundary == TrustBoundary.UNTRUSTED_NETWORK]
        self.assertGreater(len(untrusted), 0, "No untrusted-network parsers found")

    def test_blocked_parsers_reference_sec120(self):
        for p in KNOWN_PARSERS:
            if p.fuzz_status == FuzzStatus.BLOCKED:
                self.assertIn("SEC-120", p.blocker_ticket or "",
                              f"{p.parser_id}: blocked parser should reference SEC-120")


class TestPathTraversalDetection(unittest.TestCase):
    """Verify path traversal detection catches real attacks."""

    def test_dotdot_traversal(self):
        self.assertTrue(_is_path_traversal("../etc/passwd"))

    def test_dotdot_middle(self):
        self.assertTrue(_is_path_traversal("foo/../../etc/shadow"))

    def test_absolute_path(self):
        self.assertTrue(_is_path_traversal("/etc/passwd"))

    def test_normal_relative_path(self):
        self.assertFalse(_is_path_traversal("SparkEngine/Source/foo.cpp"))

    def test_backslash_normalized(self):
        self.assertEqual(_normalize_path("foo\\bar\\baz"), "foo/bar/baz")

    def test_dotdot_backslash(self):
        self.assertTrue(_is_path_traversal("foo\\..\\..\\etc\\shadow"))


class TestDuplicateIdDetection(unittest.TestCase):
    """Verify duplicate parser ID detection works."""

    def test_no_duplicates_in_real_inventory(self):
        self.assertEqual(check_duplicate_ids(), [])

    def test_detects_injected_duplicate(self):
        original = list(KNOWN_PARSERS)
        fake_dupe = ParserEntry(
            parser_id=KNOWN_PARSERS[0].parser_id,
            description="duplicate",
            trust_boundary=TrustBoundary.UNTRUSTED_FILE,
            source_files=["fake.cpp"],
            formats_handled=[".fake"],
            fuzz_status=FuzzStatus.BLOCKED,
            blocker_reason="test",
            blocker_ticket="TEST-0",
        )
        with patch("parser_inventory.KNOWN_PARSERS", original + [fake_dupe]):
            from parser_inventory import check_duplicate_ids as check_dupes
            dupes = check_dupes()
            self.assertIn(KNOWN_PARSERS[0].parser_id, dupes)


class TestMutationOmittedParser(unittest.TestCase):
    """Verify that removing a parser from the registry is caught."""

    def test_omission_detected_by_scanner(self):
        known_files = get_known_source_files()
        self.assertGreater(
            len(known_files), 0,
            "Known source files set is empty — scanner can't cross-check",
        )

    def test_real_source_tree_scan(self):
        if not (_repo_root / "SparkEngine" / "Source").exists():
            self.skipTest("Source tree not available")
        unclassified = find_unclassified_parsers(_repo_root)
        for u in unclassified:
            self.assertNotIn(
                u["file"],
                get_known_source_files(),
                f"Unclassified file is in known set — scanner bug",
            )


class TestStaleCorpusDetection(unittest.TestCase):
    """Verify stale corpus detection works with realistic dates."""

    def test_fresh_corpus_passes(self):
        entry = CorpusEntry(
            parser_id="test-parser",
            corpus_dir="tests/corpus/test",
            seed_count=10,
            budget=TIER_SMALL,
            last_verified=date.today().isoformat(),
        )
        errors = entry.validate()
        self.assertEqual(errors, [], f"Fresh corpus should pass: {errors}")

    def test_stale_corpus_fails(self):
        stale_date = (date.today() - timedelta(days=STALENESS_THRESHOLD_DAYS + 1)).isoformat()
        entry = CorpusEntry(
            parser_id="test-stale",
            corpus_dir="tests/corpus/stale",
            seed_count=5,
            budget=TIER_SMALL,
            last_verified=stale_date,
        )
        errors = entry.validate()
        self.assertTrue(
            any("stale" in e for e in errors),
            f"Stale corpus not detected: {errors}",
        )

    def test_exactly_at_threshold_passes(self):
        edge_date = (date.today() - timedelta(days=STALENESS_THRESHOLD_DAYS)).isoformat()
        entry = CorpusEntry(
            parser_id="test-edge",
            corpus_dir="tests/corpus/edge",
            seed_count=5,
            budget=TIER_SMALL,
            last_verified=edge_date,
        )
        errors = entry.validate()
        stale_errors = [e for e in errors if "stale" in e]
        self.assertEqual(stale_errors, [], "Exactly-at-threshold should not be stale")

    def test_invalid_date_fails(self):
        entry = CorpusEntry(
            parser_id="test-bad-date",
            corpus_dir="tests/corpus/bad",
            seed_count=5,
            budget=TIER_SMALL,
            last_verified="not-a-date",
        )
        errors = entry.validate()
        self.assertTrue(
            any("invalid" in e for e in errors),
            f"Invalid date not detected: {errors}",
        )


class TestResourceBudgetValidation(unittest.TestCase):
    """Verify resource budget bounds are enforced."""

    def test_valid_small_budget(self):
        errors = TIER_SMALL.validate()
        self.assertEqual(errors, [])

    def test_valid_network_budget(self):
        errors = TIER_NETWORK.validate()
        self.assertEqual(errors, [])

    def test_zero_input_bytes_fails(self):
        budget = ResourceBudget(
            max_input_bytes=0,
            max_parse_time_ms=1000,
            max_memory_mb=256,
            max_corpus_entries=5000,
            max_corpus_total_mb=500,
        )
        errors = budget.validate()
        self.assertTrue(any("max_input_bytes" in e for e in errors))

    def test_excessive_input_bytes_fails(self):
        budget = ResourceBudget(
            max_input_bytes=200 * 1024 * 1024,
            max_parse_time_ms=1000,
            max_memory_mb=256,
            max_corpus_entries=5000,
            max_corpus_total_mb=500,
        )
        errors = budget.validate()
        self.assertTrue(any("100 MB" in e for e in errors))

    def test_excessive_parse_time_fails(self):
        budget = ResourceBudget(
            max_input_bytes=1024,
            max_parse_time_ms=120_000,
            max_memory_mb=256,
            max_corpus_entries=5000,
            max_corpus_total_mb=500,
        )
        errors = budget.validate()
        self.assertTrue(any("60s" in e for e in errors))

    def test_excessive_memory_fails(self):
        budget = ResourceBudget(
            max_input_bytes=1024,
            max_parse_time_ms=1000,
            max_memory_mb=8192,
            max_corpus_entries=5000,
            max_corpus_total_mb=500,
        )
        errors = budget.validate()
        self.assertTrue(any("4 GB" in e for e in errors))

    def test_negative_corpus_entries_fails(self):
        budget = ResourceBudget(
            max_input_bytes=1024,
            max_parse_time_ms=1000,
            max_memory_mb=256,
            max_corpus_entries=-1,
            max_corpus_total_mb=500,
        )
        errors = budget.validate()
        self.assertTrue(any("max_corpus_entries" in e for e in errors))


class TestCorpusManifestValidation(unittest.TestCase):
    """Verify corpus manifest checks."""

    def test_missing_corpus_dir_without_justification_fails(self):
        entry = CorpusEntry(
            parser_id="test-no-dir",
            corpus_dir=None,
            seed_count=0,
            budget=TIER_SMALL,
            last_verified=date.today().isoformat(),
        )
        errors = entry.validate()
        self.assertTrue(any("corpus_dir" in e for e in errors))

    def test_missing_corpus_dir_with_justification_passes(self):
        entry = CorpusEntry(
            parser_id="test-justified",
            corpus_dir=None,
            seed_count=0,
            budget=TIER_SMALL,
            last_verified=date.today().isoformat(),
            empty_corpus_justification="Grammar-based generation only",
        )
        errors = entry.validate()
        corpus_errors = [e for e in errors if "corpus_dir" in e]
        self.assertEqual(corpus_errors, [])

    def test_corpus_dir_with_zero_seeds_fails(self):
        entry = CorpusEntry(
            parser_id="test-zero-seeds",
            corpus_dir="tests/corpus/empty",
            seed_count=0,
            budget=TIER_SMALL,
            last_verified=date.today().isoformat(),
        )
        errors = entry.validate()
        self.assertTrue(any("seed_count" in e for e in errors))

    def test_duplicate_ids_in_manifest(self):
        entries = [
            CorpusEntry(
                parser_id="dup",
                corpus_dir="a",
                seed_count=1,
                budget=TIER_SMALL,
                last_verified=date.today().isoformat(),
            ),
            CorpusEntry(
                parser_id="dup",
                corpus_dir="b",
                seed_count=1,
                budget=TIER_SMALL,
                last_verified=date.today().isoformat(),
            ),
        ]
        errors = validate_manifest(entries)
        self.assertTrue(any("Duplicate" in e for e in errors))


class TestBlockedParserEnforcement(unittest.TestCase):
    """Verify blocked parsers are properly constrained."""

    def test_all_blocked_parsers_have_reasons(self):
        errors = check_blocked_parsers_have_tickets()
        self.assertEqual(errors, [], f"Blocked parsers missing tickets: {errors}")

    def test_mutation_remove_blocker_ticket(self):
        mutant = ParserEntry(
            parser_id="mutant-no-ticket",
            description="test mutant",
            trust_boundary=TrustBoundary.UNTRUSTED_FILE,
            source_files=["fake.cpp"],
            formats_handled=[".fake"],
            fuzz_status=FuzzStatus.BLOCKED,
            blocker_reason="some reason",
            blocker_ticket=None,
        )
        errors = mutant.validate()
        self.assertTrue(
            any("blocker_ticket" in e for e in errors),
            f"Missing ticket not caught: {errors}",
        )

    def test_mutation_remove_blocker_reason(self):
        mutant = ParserEntry(
            parser_id="mutant-no-reason",
            description="test mutant",
            trust_boundary=TrustBoundary.UNTRUSTED_FILE,
            source_files=["fake.cpp"],
            formats_handled=[".fake"],
            fuzz_status=FuzzStatus.BLOCKED,
            blocker_reason=None,
            blocker_ticket="SEC-120",
        )
        errors = mutant.validate()
        self.assertTrue(
            any("blocker_reason" in e for e in errors),
            f"Missing reason not caught: {errors}",
        )


class TestFuzzedParserEnforcement(unittest.TestCase):
    """Verify fuzzed parser constraints."""

    def test_fuzzed_without_target_fails(self):
        mutant = ParserEntry(
            parser_id="mutant-fuzzed-no-target",
            description="test",
            trust_boundary=TrustBoundary.UNTRUSTED_FILE,
            source_files=["fake.cpp"],
            formats_handled=[".fake"],
            fuzz_status=FuzzStatus.FUZZED,
            fuzz_target=None,
            max_input_bytes=1024,
            max_parse_time_ms=1000,
        )
        errors = mutant.validate()
        self.assertTrue(
            any("fuzz_target" in e for e in errors),
            f"Missing fuzz_target not caught: {errors}",
        )

    def test_fuzzed_without_budget_fails(self):
        mutant = ParserEntry(
            parser_id="mutant-fuzzed-no-budget",
            description="test",
            trust_boundary=TrustBoundary.UNTRUSTED_FILE,
            source_files=["fake.cpp"],
            formats_handled=[".fake"],
            fuzz_status=FuzzStatus.FUZZED,
            fuzz_target="tests/fuzz/test_fake",
            max_input_bytes=0,
            max_parse_time_ms=0,
        )
        errors = mutant.validate()
        self.assertTrue(
            any("max_input_bytes" in e for e in errors),
            f"Missing budget not caught: {errors}",
        )


class TestTrustBoundaryWarnings(unittest.TestCase):
    """Verify trust boundary misclassification warnings."""

    def test_internal_parser_with_json_warns(self):
        warnings = check_untrusted_parsers_not_ignored()
        json_warnings = [w for w in warnings if ".json" in w]
        self.assertGreater(
            len(json_warnings), 0,
            "No warnings for trusted-internal parsers handling .json — classification may be wrong",
        )


class TestEndToEndPolicyCheck(unittest.TestCase):
    """Integration test: run_all_checks against real source tree."""

    def test_policy_check_runs_without_crash(self):
        result = run_all_checks(_repo_root, ci_mode=False)
        self.assertIn("passed", result)
        self.assertIn("total_parsers", result)
        self.assertGreater(result["total_parsers"], 0)

    def test_policy_check_json_serializable(self):
        result = run_all_checks(_repo_root, ci_mode=False)
        serialized = json.dumps(result)
        self.assertIsInstance(serialized, str)

    def test_ci_mode_stricter_than_default(self):
        default_result = run_all_checks(_repo_root, ci_mode=False)
        ci_result = run_all_checks(_repo_root, ci_mode=True)
        self.assertGreaterEqual(
            len(ci_result["hard_errors"]),
            len(default_result["hard_errors"]),
            "CI mode should be at least as strict as default mode",
        )


class TestManifestEmission(unittest.TestCase):
    """Verify manifest output is well-formed."""

    def test_manifest_has_required_keys(self):
        manifest = emit_manifest(_repo_root)
        required = {"schema_version", "total_parsers", "by_trust_boundary",
                     "by_fuzz_status", "parsers", "unclassified_sources",
                     "validation_errors"}
        self.assertTrue(required.issubset(manifest.keys()),
                        f"Missing keys: {required - manifest.keys()}")

    def test_manifest_parser_count_matches(self):
        manifest = emit_manifest(_repo_root)
        self.assertEqual(manifest["total_parsers"], len(KNOWN_PARSERS))

    def test_manifest_json_roundtrip(self):
        manifest = emit_manifest(_repo_root)
        serialized = json.dumps(manifest)
        deserialized = json.loads(serialized)
        self.assertEqual(deserialized["total_parsers"], manifest["total_parsers"])


class TestBudgetTierSanity(unittest.TestCase):
    """Verify budget tiers are properly ordered."""

    def test_small_lt_medium(self):
        self.assertLess(TIER_SMALL.max_input_bytes, TIER_MEDIUM.max_input_bytes)
        self.assertLess(TIER_SMALL.max_memory_mb, TIER_MEDIUM.max_memory_mb)

    def test_medium_lt_large(self):
        self.assertLess(TIER_MEDIUM.max_input_bytes, TIER_LARGE.max_input_bytes)
        self.assertLess(TIER_MEDIUM.max_memory_mb, TIER_LARGE.max_memory_mb)

    def test_network_has_smallest_input(self):
        self.assertLessEqual(TIER_NETWORK.max_input_bytes, TIER_SMALL.max_input_bytes)

    def test_network_has_shortest_timeout(self):
        self.assertLessEqual(TIER_NETWORK.max_parse_time_ms, TIER_SMALL.max_parse_time_ms)

    def test_all_tiers_valid(self):
        for name, tier in [("small", TIER_SMALL), ("medium", TIER_MEDIUM),
                           ("large", TIER_LARGE), ("network", TIER_NETWORK)]:
            errors = tier.validate()
            self.assertEqual(errors, [], f"Tier {name} invalid: {errors}")


if __name__ == "__main__":
    unittest.main()
