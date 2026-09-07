#!/usr/bin/env python3
"""Adversarial regression tests for the blocking SEC-120 policy gate."""

from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
from datetime import date, timedelta
from types import SimpleNamespace
from unittest import mock


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools" / "fuzz-policy"
sys.path.insert(0, str(TOOLS_DIR))

import check_fuzz_policy
import corpus_manifest
import parser_inventory
import policy_common


class PolicyFixture:
    def __init__(self, root: pathlib.Path) -> None:
        self.root = root
        (root / "src").mkdir(parents=True)
        (root / "tools" / "fuzz-policy").mkdir(parents=True)
        self.source = root / "src" / "ExampleParser.cpp"
        self.source.write_text("auto x = nlohmann::json::parse(input);\n", encoding="utf-8")
        self.inventory = {
            "schema_version": 1,
            "scope": {
                "roots": ["src"],
                "excluded_subtrees": [],
                "extensions": [".cpp", ".h"],
                "limits": {
                    "max_files": 10,
                    "max_directories": 10,
                    "max_file_bytes": 4096,
                    "max_total_bytes": 16384,
                    "max_depth": 4,
                    "timeout_seconds": 5,
                },
            },
            "parsers": [
                {
                    "id": "example-parser",
                    "description": "Example parser",
                    "trust_boundary": "untrusted-file",
                    "source_files": ["src/ExampleParser.cpp"],
                    "formats": [".json"],
                    "status": "blocked",
                    "blocker": {"reason": "Harness not implemented", "ticket": "SEC-120"},
                }
            ],
            "deferred_candidates": {
                "reason": "Pending classification",
                "ticket": "SEC-120",
                "source_files": [],
            },
        }
        self.corpus = {"schema_version": 1, "corpora": []}
        self.write_inventory()
        self.write_corpus()

    def write_inventory(self, raw: str | None = None) -> None:
        path = self.root / "tools" / "fuzz-policy" / "parser-inventory.json"
        path.write_text(raw if raw is not None else json.dumps(self.inventory), encoding="utf-8")

    def write_corpus(self, raw: str | None = None) -> None:
        path = self.root / "tools" / "fuzz-policy" / "corpus-manifest.json"
        path.write_text(raw if raw is not None else json.dumps(self.corpus), encoding="utf-8")

    def make_fuzzed(self) -> None:
        harness = self.root / "Tests" / "Fuzz" / "ExampleFuzz.cpp"
        harness.parent.mkdir(parents=True)
        harness.write_text(
            "constexpr int SPARK_FUZZ_MAX_DEPTH = 8;\n"
            "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }\n",
            encoding="utf-8",
        )
        cmake = self.root / "Tests" / "Fuzz" / "CMakeLists.txt"
        cmake.write_text(
            "# Tests/Fuzz/ExampleFuzz.cpp\n"
            "add_executable(FuzzExample Tests/Fuzz/ExampleFuzz.cpp)\n"
            "add_test(NAME FuzzExampleSmoke COMMAND FuzzExample -max_len=128 -timeout=1 -rss_limit_mb=64)\n"
            "set_tests_properties(FuzzExampleSmoke PROPERTIES TIMEOUT 5)\n",
            encoding="utf-8",
        )
        seeds = self.root / "Tests" / "Fuzz" / "corpus" / "example"
        seeds.mkdir(parents=True)
        (seeds / "seed.json").write_text("{}", encoding="utf-8")
        parser = self.inventory["parsers"][0]
        parser["status"] = "fuzzed"
        parser.pop("blocker")
        parser["target"] = {
            "harness": "Tests/Fuzz/ExampleFuzz.cpp",
            "cmake_file": "Tests/Fuzz/CMakeLists.txt",
            "cmake_target": "FuzzExample",
            "test_selector": "FuzzExampleSmoke",
            "corpus_id": "example-corpus",
        }
        self.corpus["corpora"] = [
            {
                "id": "example-corpus",
                "parser_id": "example-parser",
                "corpus_dir": "Tests/Fuzz/corpus/example",
                "last_verified": date.today().isoformat(),
                "budget": {
                    "max_input_bytes": 128,
                    "max_parse_time_ms": 1000,
                    "max_memory_mb": 64,
                    "max_depth": 8,
                    "max_corpus_entries": 4,
                    "max_corpus_bytes": 1024,
                    "smoke_seconds": 5,
                },
            }
        ]
        self.write_inventory()
        self.write_corpus()


class FixtureTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.fixture = PolicyFixture(self.root)

    def tearDown(self) -> None:
        self.temp.cleanup()


class TestRelativePathPolicy(unittest.TestCase):
    def test_accepts_canonical_relative_path(self) -> None:
        self.assertEqual(policy_common.normalized_relative_path("src/file.cpp", "path"), "src/file.cpp")

    def test_rejects_parent_path(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path("src/../secret", "path")

    def test_rejects_backslash_parent_path(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path(r"src\..\secret", "path")

    def test_rejects_unix_absolute_path(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path("/etc/passwd", "path")

    def test_rejects_windows_drive_absolute_path(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path("C:/Windows/win.ini", "path")

    def test_rejects_windows_drive_relative_path(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path("C:secret", "path")

    def test_rejects_unc_path(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path(r"\\server\share\secret", "path")

    def test_rejects_alternate_data_stream(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path("src/file.cpp:secret", "path")

    def test_rejects_windows_device_alias(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path("src/CON.json", "path")

    def test_rejects_windows_trailing_dot_alias(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path("src/file.cpp.", "path")

    def test_rejects_non_string(self) -> None:
        with self.assertRaises(policy_common.PolicyError):
            policy_common.normalized_relative_path(7, "path")

    def test_reparse_identity_is_rejected(self) -> None:
        identity = SimpleNamespace(st_mode=stat.S_IFREG, st_nlink=1, st_file_attributes=0x400)
        with self.assertRaises(policy_common.PolicyError):
            policy_common._validate_identity(identity, "path", expect="file")

    def test_symlink_identity_is_rejected(self) -> None:
        identity = SimpleNamespace(st_mode=stat.S_IFLNK, st_nlink=1, st_file_attributes=0)
        with self.assertRaises(policy_common.PolicyError):
            policy_common._validate_identity(identity, "path", expect="file")

    def test_hardlink_identity_is_rejected(self) -> None:
        identity = SimpleNamespace(st_mode=stat.S_IFREG, st_nlink=2, st_file_attributes=0)
        with self.assertRaises(policy_common.PolicyError):
            policy_common._validate_identity(identity, "path", expect="file")


class TestStrictJson(FixtureTestCase):
    def test_duplicate_key_is_rejected(self) -> None:
        self.fixture.write_inventory('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(policy_common.PolicyError, "duplicate key"):
            parser_inventory.load_inventory(self.root)

    def test_nan_is_rejected(self) -> None:
        self.fixture.write_inventory('{"schema_version":NaN}')
        with self.assertRaisesRegex(policy_common.PolicyError, "non-finite"):
            parser_inventory.load_inventory(self.root)

    def test_float_is_rejected(self) -> None:
        self.fixture.write_inventory('{"schema_version":1.0}')
        with self.assertRaisesRegex(policy_common.PolicyError, "non-integer"):
            parser_inventory.load_inventory(self.root)

    def test_excessive_depth_is_rejected_before_json_decode(self) -> None:
        self.fixture.write_inventory("[" * 33 + "]" * 33)
        with self.assertRaisesRegex(policy_common.PolicyError, "depth"):
            parser_inventory.load_inventory(self.root)

    def test_invalid_utf8_is_rejected(self) -> None:
        path = self.root / "tools" / "fuzz-policy" / "parser-inventory.json"
        path.write_bytes(b"\xff")
        with self.assertRaisesRegex(policy_common.PolicyError, "strict UTF-8"):
            parser_inventory.load_inventory(self.root)

    def test_bom_is_rejected(self) -> None:
        raw = json.dumps(self.fixture.inventory).encode("utf-8")
        path = self.root / "tools" / "fuzz-policy" / "parser-inventory.json"
        path.write_bytes(b"\xef\xbb\xbf" + raw)
        with self.assertRaisesRegex(policy_common.PolicyError, "BOM"):
            parser_inventory.load_inventory(self.root)

    def test_oversized_integer_is_rejected_as_policy_error(self) -> None:
        self.fixture.write_inventory('{"schema_version":' + "9" * 5000 + "}")
        with self.assertRaisesRegex(policy_common.PolicyError, "malformed JSON"):
            parser_inventory.load_inventory(self.root)


class TestInventorySchema(FixtureTestCase):
    def test_minimal_inventory_passes(self) -> None:
        report = parser_inventory.build_inventory_report(self.root)
        self.assertEqual(report["parser_count"], 1)
        self.assertEqual(report["unclassified_candidate_count"], 0)

    def test_bool_limit_is_rejected(self) -> None:
        self.fixture.inventory["scope"]["limits"]["max_files"] = True
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "must be an integer"):
            parser_inventory.load_inventory(self.root)

    def test_unknown_top_level_key_is_rejected(self) -> None:
        self.fixture.inventory["extra"] = "ignored?"
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "unknown keys"):
            parser_inventory.load_inventory(self.root)

    def test_bogus_status_is_rejected(self) -> None:
        self.fixture.inventory["parsers"][0]["status"] = "pretend"
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "blocked or fuzzed"):
            parser_inventory.load_inventory(self.root)

    def test_bogus_trust_boundary_is_rejected(self) -> None:
        self.fixture.inventory["parsers"][0]["trust_boundary"] = "trusted-ish"
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "untrusted-file"):
            parser_inventory.load_inventory(self.root)

    def test_missing_blocker_is_rejected(self) -> None:
        self.fixture.inventory["parsers"][0].pop("blocker")
        self.fixture.write_inventory()
        with self.assertRaises(policy_common.PolicyError):
            parser_inventory.load_inventory(self.root)

    def test_empty_parser_list_is_rejected(self) -> None:
        self.fixture.inventory["parsers"] = []
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "must not be empty"):
            parser_inventory.load_inventory(self.root)

    def test_directory_cannot_stand_in_for_source_file(self) -> None:
        self.fixture.inventory["parsers"][0]["source_files"] = ["src"]
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "not a regular file"):
            parser_inventory.load_inventory(self.root)

    def test_hardlinked_source_is_rejected(self) -> None:
        linked = self.root / "src" / "Hardlinked.cpp"
        os.link(self.fixture.source, linked)
        self.fixture.inventory["parsers"][0]["source_files"] = ["src/Hardlinked.cpp"]
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "hard link"):
            parser_inventory.load_inventory(self.root)


class TestBoundedScanner(FixtureTestCase):
    def test_common_json_parser_is_detected(self) -> None:
        inventory = parser_inventory.load_inventory(self.root)
        candidates = parser_inventory.scan_source_tree(self.root, inventory.scope)
        self.assertEqual(candidates[0]["source_file"], "src/ExampleParser.cpp")
        self.assertIn("json-parse", candidates[0]["reasons"])

    def test_new_candidate_fails_closed(self) -> None:
        (self.root / "src" / "NewParser.cpp").write_text("void ParseFile();\n", encoding="utf-8")
        with self.assertRaisesRegex(policy_common.PolicyError, "unclassified parser candidates"):
            parser_inventory.build_inventory_report(self.root)

    def test_stale_deferred_candidate_fails_closed(self) -> None:
        other = self.root / "src" / "Other.cpp"
        other.write_text("plain code\n", encoding="utf-8")
        self.fixture.inventory["deferred_candidates"]["source_files"] = ["src/Other.cpp"]
        self.fixture.write_inventory()
        with self.assertRaisesRegex(policy_common.PolicyError, "stale deferred"):
            parser_inventory.build_inventory_report(self.root)

    def test_unreadable_directory_is_fatal(self) -> None:
        inventory = parser_inventory.load_inventory(self.root)
        with mock.patch.object(parser_inventory.os, "scandir", side_effect=OSError("denied")):
            with self.assertRaisesRegex(policy_common.PolicyError, "unreadable"):
                parser_inventory.scan_source_tree(self.root, inventory.scope)

    def test_file_count_limit_is_fatal(self) -> None:
        (self.root / "src" / "Second.cpp").write_text("plain code\n", encoding="utf-8")
        self.fixture.inventory["scope"]["limits"]["max_files"] = 1
        self.fixture.write_inventory()
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "exceeded 1 files"):
            parser_inventory.scan_source_tree(self.root, inventory.scope)

    def test_file_size_limit_is_fatal(self) -> None:
        self.fixture.inventory["scope"]["limits"]["max_file_bytes"] = 10
        self.fixture.write_inventory()
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "source file exceeds"):
            parser_inventory.scan_source_tree(self.root, inventory.scope)

    def test_results_are_sorted(self) -> None:
        (self.root / "src" / "AParser.cpp").write_text("void ParseFile();\n", encoding="utf-8")
        inventory = parser_inventory.load_inventory(self.root)
        candidates = parser_inventory.scan_source_tree(self.root, inventory.scope)
        paths = [candidate["source_file"] for candidate in candidates]
        self.assertEqual(paths, sorted(paths))


class TestCorpusBinding(FixtureTestCase):
    def test_empty_manifest_is_valid_with_no_fuzzed_parser(self) -> None:
        inventory = parser_inventory.load_inventory(self.root)
        self.assertEqual(corpus_manifest.load_corpora(self.root, inventory), ())

    def test_valid_fuzz_target_binds_corpus_and_limits(self) -> None:
        self.fixture.make_fuzzed()
        inventory = parser_inventory.load_inventory(self.root)
        corpora = corpus_manifest.load_corpora(self.root, inventory)
        self.assertEqual(corpora[0].seed_count, 1)

    def test_missing_corpus_for_fuzzed_parser_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"] = []
        self.fixture.write_corpus()
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "corpus/parser mismatch"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_future_verification_date_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["last_verified"] = (date.today() + timedelta(days=1)).isoformat()
        self.fixture.write_corpus()
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "future"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_stale_corpus_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["last_verified"] = (date.today() - timedelta(days=91)).isoformat()
        self.fixture.write_corpus()
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "stale"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_boolean_budget_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["budget"]["max_input_bytes"] = True
        self.fixture.write_corpus()
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "must be an integer"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_float_budget_is_rejected_by_json_loader(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["budget"]["max_input_bytes"] = 1.5
        self.fixture.write_corpus()
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "non-integer"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_empty_seed_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        seed = self.root / "Tests" / "Fuzz" / "corpus" / "example" / "seed.json"
        seed.write_bytes(b"")
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "seed is empty"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_hardlinked_seed_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        seed = self.root / "Tests" / "Fuzz" / "corpus" / "example" / "seed.json"
        os.link(seed, seed.with_name("other.json"))
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "hard link"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_missing_runtime_flag_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        cmake = self.root / "Tests" / "Fuzz" / "CMakeLists.txt"
        cmake.write_text(cmake.read_text(encoding="utf-8").replace("-rss_limit_mb=64", ""), encoding="utf-8")
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "rss_limit"):
            corpus_manifest.load_corpora(self.root, inventory)

    def test_missing_depth_binding_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        harness = self.root / "Tests" / "Fuzz" / "ExampleFuzz.cpp"
        harness.write_text(harness.read_text(encoding="utf-8").replace("SPARK_FUZZ_MAX_DEPTH", "DEPTH"), encoding="utf-8")
        inventory = parser_inventory.load_inventory(self.root)
        with self.assertRaisesRegex(policy_common.PolicyError, "max_depth"):
            corpus_manifest.load_corpora(self.root, inventory)


class TestGateBehavior(FixtureTestCase):
    def test_partial_policy_passes_structural_gate_but_stays_incomplete(self) -> None:
        report = check_fuzz_policy.build_check_report(self.root, parser_inventory.DEFAULT_INVENTORY, corpus_manifest.DEFAULT_CORPUS_MANIFEST)
        self.assertTrue(report["passed"])
        self.assertFalse(report["policy_complete"])
        self.assertGreater(len(report["closure_blockers"]), 0)

    def test_emit_json_failure_returns_nonzero(self) -> None:
        self.fixture.write_inventory("{}")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = check_fuzz_policy.main(["--source-root", str(self.root), "--emit-json"])
        self.assertEqual(result, 1)
        self.assertFalse(json.loads(output.getvalue())["passed"])

    def test_invalid_corpus_emit_json_returns_nonzero(self) -> None:
        self.fixture.write_corpus("{}")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = corpus_manifest.main(["--source-root", str(self.root), "--emit-json"])
        self.assertEqual(result, 1)
        self.assertFalse(json.loads(output.getvalue())["passed"])

    def test_stale_evidence_is_fatal(self) -> None:
        evidence = self.root / "evidence.json"
        evidence.write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(policy_common.PolicyError, "stale"):
            check_fuzz_policy.validate_evidence(self.root, {"passed": True}, "evidence.json")


class TestRepositoryIntegration(unittest.TestCase):
    def test_confined_read_canonicalizes_a_relative_root(self) -> None:
        relative_root = pathlib.Path(os.path.relpath(REPO_ROOT, pathlib.Path.cwd()))
        payload = policy_common.read_confined_file(relative_root, "CMakeLists.txt", "root cmake", max_bytes=4 * 1024 * 1024)
        self.assertIn(b"cmake_minimum_required", payload)

    def test_repository_policy_report_is_structurally_valid_but_open(self) -> None:
        report = check_fuzz_policy.build_check_report(
            REPO_ROOT,
            parser_inventory.DEFAULT_INVENTORY,
            corpus_manifest.DEFAULT_CORPUS_MANIFEST,
        )
        self.assertTrue(report["passed"])
        self.assertFalse(report["policy_complete"])
        self.assertEqual(report["inventory"]["unclassified_candidate_count"], 0)

    def test_ci_and_cmake_are_blocking_wired(self) -> None:
        check_fuzz_policy.validate_ci_and_cmake_binding(REPO_ROOT)

    def test_ctest_entries_run_from_the_source_root(self) -> None:
        module = (REPO_ROOT / "cmake" / "SparkFuzzPolicy.cmake").read_text(encoding="utf-8")
        self.assertEqual(module.count('WORKING_DIRECTORY "${source_root}"'), 3)

    def test_committed_paths_use_canonical_case(self) -> None:
        tracked = subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard", "tools/fuzz-policy", "Tests/fuzz-policy"],
            cwd=REPO_ROOT,
            text=True,
            encoding="utf-8",
        ).splitlines()
        self.assertTrue(any(path == "tools/fuzz-policy/check_fuzz_policy.py" for path in tracked))
        self.assertFalse(any(path.startswith("Tools/fuzz-policy/") for path in tracked))


if __name__ == "__main__":
    unittest.main()
