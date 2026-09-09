#!/usr/bin/env python3
"""Adversarial regression tests for the blocking SEC-120 policy gate.

The fixture below is deliberately a *buildable* shape: a root listfile that
reaches the fuzz directory, a harness that includes and calls the production
entry point, a sanitizer-instrumented target, an explicit corpus argument and
real runtime limits. Every falsehood tolerated by the fixture would be a hole in
the checker, so the hostile tests each mutate exactly one of those facts.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
from datetime import date, datetime, timedelta, timezone
from types import SimpleNamespace
from unittest import mock


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools" / "fuzz-policy"
sys.path.insert(0, str(TOOLS_DIR))

import build_binding
import check_fuzz_policy
import corpus_manifest
import harness_shape
import parser_inventory
import policy_common


# Production policy evaluates expiry and corpus verification in UTC. Keep these
# tests on that same clock so they do not flip at the local/UTC date boundary.
TODAY = datetime.now(timezone.utc).date()
FUTURE = (TODAY + timedelta(days=180)).isoformat()

ROOT_CMAKE = """cmake_minimum_required(VERSION 3.25)
project(FixtureRepo NONE)
add_subdirectory(Tests/Fuzz)
"""

PARSER_SOURCE = """#include "ExampleParser.h"
#include <nlohmann/json.hpp>

bool ParseExampleDocument(const unsigned char* data, unsigned long size, int maxDepth)
{
    auto document = nlohmann::json::parse(data, data + size, nullptr, false);
    return !document.is_discarded() && maxDepth > 0;
}
"""

PARSER_HEADER = """#pragma once
bool ParseExampleDocument(const unsigned char* data, unsigned long size, int maxDepth);
"""

HARNESS = """#include <cstddef>
#include <cstdint>
#include "../../src/ExampleParser.h"

constexpr int SPARK_FUZZ_MAX_DEPTH = 8;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 128;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }
    ParseExampleDocument(data, size, SPARK_FUZZ_MAX_DEPTH);
    return 0;
}
"""

FUZZ_CMAKE = """add_executable(FuzzExampleParser ExampleFuzz.cpp ${CMAKE_SOURCE_DIR}/src/ExampleParser.cpp)
target_compile_options(FuzzExampleParser PRIVATE -fsanitize=fuzzer,address)
target_link_options(FuzzExampleParser PRIVATE -fsanitize=fuzzer,address)
add_test(NAME FuzzExampleParserSmoke
    COMMAND FuzzExampleParser -max_len=128 -timeout=1 -rss_limit_mb=64
            ${CMAKE_SOURCE_DIR}/Tests/fuzz-corpora/example)
set_tests_properties(FuzzExampleParserSmoke PROPERTIES LABELS "fuzz;security" TIMEOUT 5)
"""

SEEDS = {
    "empty-object.json": b"{}",
    "nested.json": b'{"a":{"b":[1,2,3]}}',
    "string.json": b'{"name":"spark"}',
}


def corpus_digest(root: pathlib.Path, corpus_dir: str) -> str:
    """Independent re-implementation of the manifest's seed rollup.

    Deliberately not calling corpus_manifest, so a digest test cannot pass by
    agreeing with the code it is meant to pin.
    """
    entries = []
    base = root / corpus_dir
    for path in sorted(base.rglob("*")):
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            entries.append((relative, hashlib.sha256(path.read_bytes()).hexdigest()))
    rollup = hashlib.sha256()
    for relative, digest in sorted(entries):
        rollup.update(relative.encode("utf-8"))
        rollup.update(b"\0")
        rollup.update(digest.encode("ascii"))
        rollup.update(b"\0")
    return rollup.hexdigest()


class PolicyFixture:
    """A repository shaped like one that could actually build the fuzz target."""

    def __init__(self, root: pathlib.Path) -> None:
        self.root = root
        (root / "src").mkdir(parents=True)
        (root / "tools" / "fuzz-policy").mkdir(parents=True)
        (root / "CMakeLists.txt").write_text(ROOT_CMAKE, encoding="utf-8")
        self.source = root / "src" / "ExampleParser.cpp"
        self.source.write_text(PARSER_SOURCE, encoding="utf-8")
        (root / "src" / "ExampleParser.h").write_text(PARSER_HEADER, encoding="utf-8")

        self.inventory = {
            "schema_version": 1,
            "scope": {
                "roots": ["src"],
                "excluded_subtrees": [],
                "excluded_files": [],
                "extensions": sorted(parser_inventory.ALLOWED_SOURCE_EXTENSIONS),
                "limits": {
                    "max_files": 10,
                    "max_directories": 10,
                    "max_file_bytes": 8192,
                    "max_total_bytes": 65536,
                    "max_depth": 4,
                    "timeout_seconds": 30,
                },
            },
            "parsers": [
                {
                    "id": "example-parser",
                    "description": "Example parser",
                    "trust_boundary": "untrusted-file",
                    "source_files": ["src/ExampleParser.cpp", "src/ExampleParser.h"],
                    "formats": [".json"],
                    "status": "blocked",
                    "blocker": {"reason": "Harness not implemented", "ticket": "SEC-120"},
                }
            ],
            "deferred_candidates": [],
        }
        self.corpus = {"schema_version": 1, "corpora": []}
        self.write_inventory()
        self.write_corpus()

    # -- writers ---------------------------------------------------------
    def write_inventory(self, raw: str | None = None) -> None:
        path = self.root / "tools" / "fuzz-policy" / "parser-inventory.json"
        path.write_text(raw if raw is not None else json.dumps(self.inventory), encoding="utf-8")

    def write_corpus(self, raw: str | None = None) -> None:
        path = self.root / "tools" / "fuzz-policy" / "corpus-manifest.json"
        path.write_text(raw if raw is not None else json.dumps(self.corpus), encoding="utf-8")

    @property
    def fuzz_cmake(self) -> pathlib.Path:
        return self.root / "Tests" / "Fuzz" / "CMakeLists.txt"

    @property
    def harness(self) -> pathlib.Path:
        return self.root / "Tests" / "Fuzz" / "ExampleFuzz.cpp"

    def rewrite_cmake(self, text: str) -> None:
        self.fuzz_cmake.write_text(text, encoding="utf-8")

    def rewrite_harness(self, text: str) -> None:
        self.harness.write_text(text, encoding="utf-8")

    def load(self) -> parser_inventory.Inventory:
        return parser_inventory.load_inventory(self.root)

    def load_corpora(self):
        return corpus_manifest.load_corpora(self.root, self.load())

    # -- promotion to a fuzzed parser -------------------------------------
    def make_fuzzed(self) -> None:
        fuzz_dir = self.root / "Tests" / "Fuzz"
        fuzz_dir.mkdir(parents=True)
        self.harness.write_text(HARNESS, encoding="utf-8")
        self.fuzz_cmake.write_text(FUZZ_CMAKE, encoding="utf-8")

        seeds = self.root / "Tests" / "fuzz-corpora" / "example"
        seeds.mkdir(parents=True)
        for name, payload in SEEDS.items():
            (seeds / name).write_bytes(payload)

        parser = self.inventory["parsers"][0]
        parser["status"] = "fuzzed"
        parser.pop("blocker")
        parser["target"] = {
            "harness": "Tests/Fuzz/ExampleFuzz.cpp",
            "cmake_file": "Tests/Fuzz/CMakeLists.txt",
            "cmake_target": "FuzzExampleParser",
            "test_selector": "FuzzExampleParserSmoke",
            "corpus_id": "example-corpus",
            "entry_symbol": "ParseExampleDocument",
        }
        self.corpus["corpora"] = [
            {
                "id": "example-corpus",
                "parser_id": "example-parser",
                "corpus_dir": "Tests/fuzz-corpora/example",
                "last_verified": TODAY.isoformat(),
                "content_digest": corpus_digest(self.root, "Tests/fuzz-corpora/example"),
                "budget": {
                    "max_input_bytes": 128,
                    "max_parse_time_ms": 1000,
                    "max_memory_mb": 64,
                    "max_depth": 8,
                    "max_corpus_entries": 8,
                    "max_corpus_bytes": 4096,
                    "smoke_seconds": 5,
                },
            }
        ]
        self.write_inventory()
        self.write_corpus()

    def refresh_digest(self) -> None:
        self.corpus["corpora"][0]["content_digest"] = corpus_digest(self.root, "Tests/fuzz-corpora/example")
        self.write_corpus()


class FixtureTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.fixture = PolicyFixture(self.root)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def assertPolicyError(self, pattern: str):
        return self.assertRaisesRegex(policy_common.PolicyError, pattern)


# =========================================================================
# Path and identity policy
# =========================================================================
class TestRelativePathPolicy(unittest.TestCase):
    def test_accepts_canonical_relative_path(self) -> None:
        self.assertEqual(policy_common.normalized_relative_path("src/file.cpp", "path"), "src/file.cpp")

    def test_rejects_parent_path(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "empty, dot, or parent component"):
            policy_common.normalized_relative_path("src/../secret", "path")

    def test_rejects_backslash_parent_path(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "empty, dot, or parent component"):
            policy_common.normalized_relative_path(r"src\..\secret", "path")

    def test_rejects_unix_absolute_path(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "must be repository-relative"):
            policy_common.normalized_relative_path("/etc/passwd", "path")

    def test_rejects_windows_drive_absolute_path(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "must be repository-relative"):
            policy_common.normalized_relative_path("C:/Windows/win.ini", "path")

    def test_rejects_windows_drive_relative_path(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "must be repository-relative"):
            policy_common.normalized_relative_path("C:secret", "path")

    def test_rejects_unc_path(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "must be repository-relative"):
            policy_common.normalized_relative_path(r"\\server\share\secret", "path")

    def test_rejects_alternate_data_stream(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "drive/stream alias"):
            policy_common.normalized_relative_path("src/file.cpp:secret", "path")

    def test_rejects_windows_device_alias(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "Windows device alias"):
            policy_common.normalized_relative_path("src/CON.json", "path")

    def test_rejects_windows_trailing_dot_alias(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "Windows-normalized alias"):
            policy_common.normalized_relative_path("src/file.cpp.", "path")

    def test_rejects_non_string(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "must be a non-empty string"):
            policy_common.normalized_relative_path(7, "path")

    def test_reparse_identity_is_rejected(self) -> None:
        identity = SimpleNamespace(st_mode=stat.S_IFREG, st_nlink=1, st_file_attributes=0x400)
        with self.assertRaisesRegex(policy_common.PolicyError, "is a reparse point"):
            policy_common._validate_identity(identity, "path", expect="file")

    def test_symlink_identity_is_rejected(self) -> None:
        identity = SimpleNamespace(st_mode=stat.S_IFLNK, st_nlink=1, st_file_attributes=0)
        with self.assertRaisesRegex(policy_common.PolicyError, "not a regular file"):
            policy_common._validate_identity(identity, "path", expect="file")

    def test_hardlink_identity_is_rejected(self) -> None:
        identity = SimpleNamespace(st_mode=stat.S_IFREG, st_nlink=2, st_file_attributes=0)
        with self.assertRaisesRegex(policy_common.PolicyError, "must not be a hard link"):
            policy_common._validate_identity(identity, "path", expect="file")


class TestTokenPolicy(unittest.TestCase):
    def test_case_aliased_values_are_rejected(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "case-aliased duplicates"):
            policy_common.casefold_duplicates(["Alpha", "alpha"], "field")

    def test_non_ascii_homoglyph_token_is_rejected(self) -> None:
        # Cyrillic 'а' in "example".
        with self.assertRaisesRegex(policy_common.PolicyError, "must be ASCII"):
            policy_common.require_token("ex\u0430mple", "field", parser_inventory.ID_PATTERN)

    def test_padded_token_is_rejected(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "surrounding whitespace|non-empty"):
            policy_common.require_token(" example ", "field", parser_inventory.ID_PATTERN)

    def test_iso_date_must_be_canonical(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "must be YYYY-MM-DD"):
            policy_common.require_iso_date("2026-8-1", "field")


# =========================================================================
# Strict JSON
# =========================================================================
class TestStrictJson(FixtureTestCase):
    def test_duplicate_key_is_rejected(self) -> None:
        self.fixture.write_inventory('{"schema_version":1,"schema_version":1}')
        with self.assertPolicyError("duplicate key"):
            self.fixture.load()

    def test_nan_is_rejected(self) -> None:
        self.fixture.write_inventory('{"schema_version":NaN}')
        with self.assertPolicyError("non-finite"):
            self.fixture.load()

    def test_float_is_rejected(self) -> None:
        self.fixture.write_inventory('{"schema_version":1.0}')
        with self.assertPolicyError("non-integer"):
            self.fixture.load()

    def test_excessive_depth_is_rejected_before_json_decode(self) -> None:
        self.fixture.write_inventory("[" * 33 + "]" * 33)
        with self.assertPolicyError("exceeds JSON depth"):
            self.fixture.load()

    def test_invalid_utf8_is_rejected(self) -> None:
        (self.root / "tools" / "fuzz-policy" / "parser-inventory.json").write_bytes(b"\xff")
        with self.assertPolicyError("must be strict UTF-8"):
            self.fixture.load()

    def test_bom_is_rejected(self) -> None:
        raw = json.dumps(self.fixture.inventory).encode("utf-8")
        (self.root / "tools" / "fuzz-policy" / "parser-inventory.json").write_bytes(b"\xef\xbb\xbf" + raw)
        with self.assertPolicyError("must not contain a UTF-8 BOM"):
            self.fixture.load()

    def test_oversized_integer_is_rejected_as_policy_error(self) -> None:
        self.fixture.write_inventory('{"schema_version":' + "9" * 5000 + "}")
        with self.assertPolicyError("malformed JSON"):
            self.fixture.load()


# =========================================================================
# Inventory schema
# =========================================================================
class TestInventorySchema(FixtureTestCase):
    def test_minimal_inventory_passes(self) -> None:
        report = parser_inventory.build_inventory_report(self.root)
        self.assertEqual(report["parser_count"], 1)
        self.assertEqual(report["candidate_count"], 2)
        self.assertEqual(report["unclassified_candidate_count"], 0)

    def test_bool_limit_is_rejected(self) -> None:
        self.fixture.inventory["scope"]["limits"]["max_files"] = True
        self.fixture.write_inventory()
        with self.assertPolicyError(r"inventory\.scope\.limits\.max_files must be an integer"):
            self.fixture.load()

    def test_unknown_top_level_key_is_rejected(self) -> None:
        self.fixture.inventory["extra"] = "ignored?"
        self.fixture.write_inventory()
        with self.assertPolicyError("inventory has unknown keys: extra"):
            self.fixture.load()

    def test_bogus_status_is_rejected(self) -> None:
        self.fixture.inventory["parsers"][0]["status"] = "pretend"
        self.fixture.write_inventory()
        with self.assertPolicyError(r"parsers\[0\]\.status does not match"):
            self.fixture.load()

    def test_bogus_trust_boundary_is_rejected(self) -> None:
        self.fixture.inventory["parsers"][0]["trust_boundary"] = "trusted-ish"
        self.fixture.write_inventory()
        with self.assertPolicyError(r"parsers\[0\]\.trust_boundary does not match"):
            self.fixture.load()

    def test_missing_blocker_is_rejected(self) -> None:
        self.fixture.inventory["parsers"][0].pop("blocker")
        self.fixture.write_inventory()
        with self.assertPolicyError(r"parsers\[0\]\.blocker must be an object"):
            self.fixture.load()

    def test_empty_parser_list_is_rejected(self) -> None:
        self.fixture.inventory["parsers"] = []
        self.fixture.write_inventory()
        with self.assertPolicyError("inventory.parsers must not be empty"):
            self.fixture.load()

    def test_directory_cannot_stand_in_for_source_file(self) -> None:
        self.fixture.inventory["parsers"][0]["source_files"] = ["src"]
        self.fixture.write_inventory()
        with self.assertPolicyError("is not a regular file"):
            self.fixture.load()

    def test_hardlinked_source_is_rejected(self) -> None:
        linked = self.root / "src" / "Hardlinked.cpp"
        try:
            os.link(self.fixture.source, linked)
        except OSError as exc:  # pragma: no cover - filesystem dependent
            self.fail(f"hard-link fixture unavailable on this filesystem: {exc}")
        self.fixture.inventory["parsers"][0]["source_files"] = ["src/Hardlinked.cpp"]
        self.fixture.write_inventory()
        with self.assertPolicyError("must not be a hard link"):
            self.fixture.load()

    def test_case_variant_source_cannot_be_owned_twice(self) -> None:
        self.fixture.inventory["parsers"][0]["source_files"] = [
            "src/ExampleParser.cpp",
            "src/exampleparser.cpp",
        ]
        self.fixture.write_inventory()
        with self.assertPolicyError("case-aliased duplicates"):
            self.fixture.load()

    def test_scan_roots_must_cover_first_party_source_trees(self) -> None:
        (self.root / "SparkWidget" / "src").mkdir(parents=True)
        (self.root / "SparkWidget" / "src" / "Widget.cpp").write_text("int main(){}\n", encoding="utf-8")
        self.fixture.write_inventory()
        with self.assertPolicyError("does not cover first-party source trees: SparkWidget/src"):
            self.fixture.load()

    def test_extension_set_may_not_be_narrowed(self) -> None:
        self.fixture.inventory["scope"]["extensions"] = [".cpp", ".h"]
        self.fixture.write_inventory()
        with self.assertPolicyError("must be the full supported set"):
            self.fixture.load()


# =========================================================================
# Exclusions and deferrals
# =========================================================================
def _exclusion(path: str, ticket: str = "NET-100", owner: str = "net-100-protocol-campaign", hidden: int = 1) -> dict:
    return {
        "path": path,
        "reason": "reviewed",
        "ticket": ticket,
        "owner": owner,
        "expires": FUTURE,
        "hidden_candidate_count": hidden,
    }


class TestExclusionPolicy(FixtureTestCase):
    def setUp(self) -> None:
        super().setUp()
        nested = self.root / "src" / "net"
        nested.mkdir()
        (nested / "PacketParser.cpp").write_text(
            'auto v = nlohmann::json::parse(data);\n', encoding="utf-8"
        )

    def test_excluding_an_entire_scan_root_is_rejected(self) -> None:
        self.fixture.inventory["scope"]["excluded_subtrees"] = [_exclusion("src")]
        self.fixture.write_inventory()
        with self.assertPolicyError("excludes an entire scan root"):
            self.fixture.load()

    def test_unapproved_ticket_namespace_is_rejected(self) -> None:
        self.fixture.inventory["scope"]["excluded_subtrees"] = [
            _exclusion("src/net", ticket="ANY-1", owner="net-100-protocol-campaign")
        ]
        self.fixture.write_inventory()
        with self.assertPolicyError("is not an approved exclusion ticket"):
            self.fixture.load()

    def test_wrong_owner_for_approved_ticket_is_rejected(self) -> None:
        self.fixture.inventory["scope"]["excluded_subtrees"] = [_exclusion("src/net", owner="someone-else")]
        self.fixture.write_inventory()
        with self.assertPolicyError("owner must be net-100-protocol-campaign"):
            self.fixture.load()

    def test_expired_exclusion_is_rejected(self) -> None:
        exclusion = _exclusion("src/net")
        exclusion["expires"] = (TODAY - timedelta(days=1)).isoformat()
        self.fixture.inventory["scope"]["excluded_subtrees"] = [exclusion]
        self.fixture.write_inventory()
        with self.assertPolicyError("expired on"):
            self.fixture.load()

    def test_exclusion_without_expiry_is_rejected(self) -> None:
        exclusion = _exclusion("src/net")
        exclusion.pop("expires")
        self.fixture.inventory["scope"]["excluded_subtrees"] = [exclusion]
        self.fixture.write_inventory()
        with self.assertPolicyError("is missing keys: expires"):
            self.fixture.load()

    def test_overlapping_exclusions_are_rejected(self) -> None:
        (self.root / "src" / "net" / "inner").mkdir()
        (self.root / "src" / "net" / "inner" / "Deep.cpp").write_text("void ParseFile();\n", encoding="utf-8")
        self.fixture.inventory["scope"]["excluded_subtrees"] = [
            _exclusion("src/net"),
            _exclusion("src/net/inner"),
        ]
        self.fixture.write_inventory()
        with self.assertPolicyError("overlap: src/net contains src/net/inner"):
            self.fixture.load()

    def test_exclusion_hiding_nothing_is_stale(self) -> None:
        empty = self.root / "src" / "empty"
        empty.mkdir()
        (empty / "Plain.cpp").write_text("int plain = 1;\n", encoding="utf-8")
        self.fixture.inventory["scope"]["excluded_subtrees"] = [
            _exclusion("src/net"),
            _exclusion("src/empty"),
        ]
        self.fixture.write_inventory()
        with self.assertPolicyError("hides no parser candidates and is stale"):
            parser_inventory.build_inventory_report(self.root)

    def test_exclusion_cannot_silently_absorb_a_new_parser(self) -> None:
        self.fixture.inventory["scope"]["excluded_subtrees"] = [_exclusion("src/net", hidden=1)]
        self.fixture.write_inventory()
        parser_inventory.build_inventory_report(self.root)  # baseline: one hidden candidate
        (self.root / "src" / "net" / "NewEvilParser.cpp").write_text(
            'auto v = nlohmann::json::parse(data);\n', encoding="utf-8"
        )
        with self.assertPolicyError(r"hides 2 candidates but declares 1"):
            parser_inventory.build_inventory_report(self.root)

    def test_file_exclusion_requires_a_matching_digest(self) -> None:
        self.fixture.inventory["scope"]["excluded_files"] = [
            {
                "path": "src/net/PacketParser.cpp",
                "reason": "reviewed line by line",
                "ticket": "NET-100",
                "owner": "net-100-protocol-campaign",
                "expires": FUTURE,
                "sha256": "0" * 64,
            }
        ]
        self.fixture.write_inventory()
        with self.assertPolicyError("no longer matches its reviewed digest"):
            parser_inventory.build_inventory_report(self.root)

    def test_file_exclusion_with_the_reviewed_digest_is_accepted(self) -> None:
        payload = (self.root / "src" / "net" / "PacketParser.cpp").read_bytes()
        self.fixture.inventory["scope"]["excluded_files"] = [
            {
                "path": "src/net/PacketParser.cpp",
                "reason": "reviewed line by line",
                "ticket": "NET-100",
                "owner": "net-100-protocol-campaign",
                "expires": FUTURE,
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        ]
        self.fixture.write_inventory()
        report = parser_inventory.build_inventory_report(self.root)
        self.assertEqual(len(report["excluded_files"]), 1)
        self.assertEqual(report["unclassified_candidate_count"], 0)


class TestDeferralPolicy(FixtureTestCase):
    def setUp(self) -> None:
        super().setUp()
        (self.root / "src" / "NewParser.cpp").write_text('json::parse(text);\n', encoding="utf-8")

    def _defer(self, **overrides) -> None:
        entry = {
            "source_file": "src/NewParser.cpp",
            "reason": "Detected by json-parse; awaiting classification.",
            "owner": "sec-120-parser-triage",
            "ticket": "SEC-120",
            "expires": FUTURE,
        }
        entry.update(overrides)
        self.fixture.inventory["deferred_candidates"] = [entry]
        self.fixture.write_inventory()

    def test_new_candidate_fails_closed(self) -> None:
        with self.assertPolicyError("unclassified parser candidates: src/NewParser.cpp"):
            parser_inventory.build_inventory_report(self.root)

    def test_deferral_with_owner_and_expiry_is_accepted(self) -> None:
        self._defer()
        report = parser_inventory.build_inventory_report(self.root)
        self.assertEqual(report["deferred_candidate_count"], 1)

    def test_expired_deferral_is_rejected(self) -> None:
        self._defer(expires=(TODAY - timedelta(days=1)).isoformat())
        with self.assertPolicyError("deferral expired on"):
            self.fixture.load()

    def test_deferral_without_the_named_owner_is_rejected(self) -> None:
        self._defer(owner="somebody")
        with self.assertPolicyError("owner must be sec-120-parser-triage"):
            self.fixture.load()

    def test_deferral_under_a_foreign_ticket_is_rejected(self) -> None:
        self._defer(ticket="ANY-1")
        with self.assertPolicyError("ticket must be SEC-120"):
            self.fixture.load()

    def test_stale_deferred_candidate_fails_closed(self) -> None:
        (self.root / "src" / "Other.cpp").write_text("plain code\n", encoding="utf-8")
        self._defer()
        stale = dict(self.fixture.inventory["deferred_candidates"][0])
        stale["source_file"] = "src/Other.cpp"
        self.fixture.inventory["deferred_candidates"].append(stale)
        self.fixture.write_inventory()
        with self.assertPolicyError("stale deferred parser candidates: src/Other.cpp"):
            parser_inventory.build_inventory_report(self.root)


# =========================================================================
# Bounded scanning
# =========================================================================
class TestBoundedScanner(FixtureTestCase):
    def test_strict_json_entry_point_is_detected(self) -> None:
        (self.root / "src" / "Strict.cpp").write_text(
            "auto doc = Spark::Json::ParseStrict(text);\n", encoding="utf-8"
        )
        scan = parser_inventory.scan_source_tree(self.root, self.fixture.load().scope)
        reasons = {item["source_file"]: item["reasons"] for item in scan.candidates}
        self.assertIn("json-parse-strict", reasons["src/Strict.cpp"])

    def test_generic_binary_reader_is_detected(self) -> None:
        (self.root / "src" / "Blob.cpp").write_text(
            "void Read(std::ifstream& in, Header& h) { in.read(reinterpret_cast<char*>(&h), sizeof(h)); }\n",
            encoding="utf-8",
        )
        scan = parser_inventory.scan_source_tree(self.root, self.fixture.load().scope)
        reasons = {item["source_file"]: item["reasons"] for item in scan.candidates}
        self.assertIn("binary-stream-read", reasons["src/Blob.cpp"])

    def test_unreadable_directory_is_fatal(self) -> None:
        scope = self.fixture.load().scope
        with mock.patch.object(policy_common.os, "scandir", side_effect=OSError("denied")):
            with self.assertPolicyError("source scan is unreadable"):
                parser_inventory.scan_source_tree(self.root, scope)

    def test_file_count_limit_is_fatal(self) -> None:
        (self.root / "src" / "Second.cpp").write_text("plain code\n", encoding="utf-8")
        self.fixture.inventory["scope"]["limits"]["max_files"] = 1
        self.fixture.write_inventory()
        with self.assertPolicyError("source scan exceeded 1 files"):
            parser_inventory.scan_source_tree(self.root, self.fixture.load().scope)

    def test_file_size_limit_is_fatal(self) -> None:
        self.fixture.inventory["scope"]["limits"]["max_file_bytes"] = 10
        self.fixture.write_inventory()
        with self.assertPolicyError("source file exceeds 10 bytes"):
            parser_inventory.scan_source_tree(self.root, self.fixture.load().scope)

    def test_directory_entries_are_bounded_before_materialization(self) -> None:
        crowded = self.root / "src" / "crowded"
        crowded.mkdir()
        for index in range(40):
            (crowded / f"note{index}.txt").write_text("x", encoding="utf-8")
        with self.assertPolicyError("exceeds 8 directory entries"):
            policy_common.bounded_scandir(crowded, "probe", max_entries=8)

    def test_wall_clock_deadline_is_enforced(self) -> None:
        expired = policy_common.Deadline(0, "source scan")
        with self.assertPolicyError("source scan exceeded 0 seconds"):
            parser_inventory.scan_source_tree(self.root, self.fixture.load().scope, deadline=expired)

    def test_results_are_sorted_across_roots(self) -> None:
        (self.root / "zzz").mkdir()
        (self.root / "zzz" / "AAA.cpp").write_text("json::parse(x);\n", encoding="utf-8")
        (self.root / "src" / "zzz.cpp").write_text("json::parse(x);\n", encoding="utf-8")
        self.fixture.inventory["scope"]["roots"] = ["zzz", "src"]
        self.fixture.write_inventory()
        scan = parser_inventory.scan_source_tree(self.root, self.fixture.load().scope)
        paths = [item["source_file"] for item in scan.candidates]
        self.assertEqual(paths, sorted(paths))
        self.assertIn("zzz/AAA.cpp", paths)

    def test_detector_blind_spots_are_reported_not_hidden(self) -> None:
        quiet = self.root / "src" / "Quiet.cpp"
        quiet.write_text("int quiet = 1;\n", encoding="utf-8")
        self.fixture.inventory["parsers"][0]["source_files"].append("src/Quiet.cpp")
        self.fixture.write_inventory()
        report = parser_inventory.build_inventory_report(self.root)
        self.assertEqual(report["detector_blind_spot_count"], 1)
        self.assertEqual(report["detector_blind_spots"], ["src/Quiet.cpp"])


# =========================================================================
# CMake tokenisation
# =========================================================================
class TestCMakeTokenizer(unittest.TestCase):
    def test_line_comment_is_not_a_command(self) -> None:
        commands = build_binding.parse_cmake("# add_executable(Ghost ghost.cpp)\n", "f")
        self.assertEqual(commands, ())

    def test_bracket_comment_is_not_a_command(self) -> None:
        commands = build_binding.parse_cmake("#[[\nadd_executable(Ghost ghost.cpp)\n]]\n", "f")
        self.assertEqual(commands, ())

    def test_quoted_hash_is_preserved(self) -> None:
        commands = build_binding.parse_cmake('message(STATUS "a # b")\n', "f")
        self.assertEqual(commands[0].arguments, ("STATUS", "a # b"))

    def test_unterminated_argument_list_is_fatal(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "unterminated argument list"):
            build_binding.parse_cmake("add_executable(Ghost\n", "f")

    def test_unresolvable_variable_is_refused(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "unresolvable CMake variable"):
            build_binding._literal("${MYSTERY}", "f")


# =========================================================================
# Harness shape
# =========================================================================
class TestHarnessShape(unittest.TestCase):
    def test_comments_and_literals_are_stripped(self) -> None:
        stripped = harness_shape.strip_cxx(
            '// LLVMFuzzerTestOneInput\nconst char* k = "LLVMFuzzerTestOneInput";\n'
        )
        self.assertNotIn("LLVMFuzzerTestOneInput", stripped)

    def test_raw_string_body_is_stripped(self) -> None:
        stripped = harness_shape.strip_cxx('auto s = R"json(LLVMFuzzerTestOneInput)json";\n')
        self.assertNotIn("LLVMFuzzerTestOneInput", stripped)

    def test_block_comment_body_is_stripped(self) -> None:
        stripped = harness_shape.strip_cxx("/* SPARK_FUZZ_MAX_DEPTH = 8 */\nint x;\n")
        self.assertNotIn("SPARK_FUZZ_MAX_DEPTH", stripped)

    def test_unterminated_block_comment_is_fatal(self) -> None:
        with self.assertRaisesRegex(policy_common.PolicyError, "unterminated block comment"):
            harness_shape.strip_cxx("/* never closed\n")


# =========================================================================
# Corpus and target binding
# =========================================================================
class TestCorpusBinding(FixtureTestCase):
    def test_empty_manifest_is_valid_with_no_fuzzed_parser(self) -> None:
        self.assertEqual(self.fixture.load_corpora(), ())

    def test_valid_fuzz_target_binds_corpus_and_limits(self) -> None:
        self.fixture.make_fuzzed()
        corpora = self.fixture.load_corpora()
        self.assertEqual(len(corpora), 1)
        self.assertEqual(corpora[0].seed_count, len(SEEDS))
        self.assertEqual(corpora[0].seed_bytes, sum(len(payload) for payload in SEEDS.values()))

    # -- the checker must validate a build, not prose ----------------------
    def test_comment_only_cmake_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        commented = "".join(f"# {line}\n" for line in FUZZ_CMAKE.splitlines())
        self.fixture.rewrite_cmake(commented)
        with self.assertPolicyError("has no add_executable declaring target 'FuzzExampleParser'"):
            self.fixture.load_corpora()

    def test_literal_blob_comment_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(
            "# FuzzExampleParser FuzzExampleParserSmoke Tests/Fuzz/ExampleFuzz.cpp "
            "-max_len=128 -timeout=1 -rss_limit_mb=64 TIMEOUT 5 fsanitize=fuzzer\n"
        )
        with self.assertPolicyError("has no add_executable"):
            self.fixture.load_corpora()

    def test_undeclared_cmake_target_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(FUZZ_CMAKE.replace("add_executable(FuzzExampleParser", "add_executable(SomethingElse"))
        with self.assertPolicyError("has no add_executable declaring target 'FuzzExampleParser'"):
            self.fixture.load_corpora()

    def test_target_must_compile_the_declared_harness(self) -> None:
        self.fixture.make_fuzzed()
        (self.root / "Tests" / "Fuzz" / "Other.cpp").write_text("int other;\n", encoding="utf-8")
        self.fixture.rewrite_cmake(FUZZ_CMAKE.replace("ExampleFuzz.cpp ", "Other.cpp ", 1))
        with self.assertPolicyError("does not compile the declared harness"):
            self.fixture.load_corpora()

    def test_relative_harness_path_must_resolve_from_the_listfile(self) -> None:
        self.fixture.make_fuzzed()
        # The repo-relative spelling would resolve to Tests/Fuzz/Tests/Fuzz/...
        self.fixture.rewrite_cmake(FUZZ_CMAKE.replace("ExampleFuzz.cpp ", "Tests/Fuzz/ExampleFuzz.cpp ", 1))
        with self.assertPolicyError("does not compile the declared harness"):
            self.fixture.load_corpora()

    def test_add_test_must_invoke_the_declared_target(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(FUZZ_CMAKE.replace("COMMAND FuzzExampleParser ", "COMMAND some_other_binary "))
        with self.assertPolicyError("does not run target 'FuzzExampleParser'"):
            self.fixture.load_corpora()

    def test_target_file_generator_expression_is_accepted(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(
            FUZZ_CMAKE.replace("COMMAND FuzzExampleParser ", "COMMAND $<TARGET_FILE:FuzzExampleParser> ")
        )
        self.assertEqual(len(self.fixture.load_corpora()), 1)

    def test_limits_outside_add_test_are_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(
            FUZZ_CMAKE.replace("-max_len=128", "-max_len=999999")
            + 'message(STATUS "-max_len=128 -timeout=1 -rss_limit_mb=64")\n'
        )
        with self.assertPolicyError("does not pass -max_len=128"):
            self.fixture.load_corpora()

    def test_inflated_ctest_timeout_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(FUZZ_CMAKE.replace("TIMEOUT 5", "TIMEOUT 500"))
        with self.assertPolicyError("does not set TIMEOUT 5"):
            self.fixture.load_corpora()

    def test_missing_fuzz_label_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(FUZZ_CMAKE.replace('"fuzz;security"', '"security"'))
        with self.assertPolicyError("is not labelled 'fuzz'"):
            self.fixture.load_corpora()

    def test_missing_sanitizer_instrumentation_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(
            "\n".join(line for line in FUZZ_CMAKE.splitlines() if "fsanitize" not in line) + "\n"
        )
        with self.assertPolicyError("is not built with -fsanitize=fuzzer"):
            self.fixture.load_corpora()

    def test_corpus_dir_must_be_passed_to_the_target(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_cmake(
            FUZZ_CMAKE.replace("\n            ${CMAKE_SOURCE_DIR}/Tests/fuzz-corpora/example", "")
        )
        with self.assertPolicyError("does not pass the corpus directory"):
            self.fixture.load_corpora()

    def test_unreviewed_input_path_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        (self.root / "Tests" / "fuzz-corpora" / "other").mkdir()
        self.fixture.rewrite_cmake(
            FUZZ_CMAKE.replace(
                "${CMAKE_SOURCE_DIR}/Tests/fuzz-corpora/example",
                "${CMAKE_SOURCE_DIR}/Tests/fuzz-corpora/example ${CMAKE_SOURCE_DIR}/Tests/fuzz-corpora/other",
            )
        )
        with self.assertPolicyError("passes unreviewed input paths"):
            self.fixture.load_corpora()

    def test_orphan_cmake_file_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\nproject(FixtureRepo NONE)\n", encoding="utf-8"
        )
        with self.assertPolicyError("is not reachable from CMakeLists.txt"):
            self.fixture.load_corpora()

    # -- harness shape -----------------------------------------------------
    def test_harness_entrypoint_in_comment_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_harness(
            "// int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {}\n"
            "#include <cstdint>\n"
            "constexpr int SPARK_FUZZ_MAX_DEPTH = 8;\n"
            "constexpr int SPARK_FUZZ_MAX_INPUT_BYTES = 128;\n"
        )
        with self.assertPolicyError("does not define LLVMFuzzerTestOneInput"):
            self.fixture.load_corpora()

    def test_harness_entrypoint_in_string_literal_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_harness(
            "#include <cstdint>\n"
            'const char* k = "int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {";\n'
        )
        with self.assertPolicyError("does not define LLVMFuzzerTestOneInput"):
            self.fixture.load_corpora()

    def test_harness_declaration_without_a_body_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_harness(
            HARNESS.replace("size_t size)\n{", "size_t size);\nint unused()\n{").replace(
                "    return 0;\n}\n", "    return 0;\n}\n", 1
            )
        )
        with self.assertPolicyError("does not define LLVMFuzzerTestOneInput"):
            self.fixture.load_corpora()

    def test_declared_depth_must_be_used(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_harness(HARNESS.replace("SPARK_FUZZ_MAX_DEPTH);", "8);"))
        with self.assertPolicyError("defines SPARK_FUZZ_MAX_DEPTH but never uses it"):
            self.fixture.load_corpora()

    def test_depth_value_must_match_the_budget(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_harness(HARNESS.replace("SPARK_FUZZ_MAX_DEPTH = 8", "SPARK_FUZZ_MAX_DEPTH = 9"))
        with self.assertPolicyError("does not define SPARK_FUZZ_MAX_DEPTH = 8"):
            self.fixture.load_corpora()

    def test_harness_must_bound_its_input_size(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_harness(
            HARNESS.replace("if (size > SPARK_FUZZ_MAX_INPUT_BYTES)", "if (SPARK_FUZZ_MAX_INPUT_BYTES == 0)")
        )
        with self.assertPolicyError("never compares its input size against SPARK_FUZZ_MAX_INPUT_BYTES"):
            self.fixture.load_corpora()

    def test_harness_must_call_the_declared_entry_symbol(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.rewrite_harness(HARNESS.replace("ParseExampleDocument(data, size, SPARK_FUZZ_MAX_DEPTH);", "(void)SPARK_FUZZ_MAX_DEPTH;"))
        with self.assertPolicyError("never calls the declared entry symbol"):
            self.fixture.load_corpora()

    def test_entry_symbol_must_exist_in_the_parser_sources(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.inventory["parsers"][0]["target"]["entry_symbol"] = "ParseSomethingElse"
        self.fixture.rewrite_harness(HARNESS.replace("ParseExampleDocument(", "ParseSomethingElse("))
        self.fixture.write_inventory()
        with self.assertPolicyError("is not declared by any inventoried source file"):
            self.fixture.load_corpora()

    # -- artifact shapes ---------------------------------------------------
    def test_harness_may_not_be_a_declared_parser_source(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.inventory["parsers"][0]["target"]["harness"] = "src/ExampleParser.cpp"
        self.fixture.write_inventory()
        with self.assertPolicyError("is also an inventoried parser source"):
            self.fixture.load_corpora()

    def test_cmake_file_must_be_a_cmake_listfile(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.inventory["parsers"][0]["target"]["cmake_file"] = "Tests/Fuzz/ExampleFuzz.cpp"
        self.fixture.write_inventory()
        with self.assertPolicyError("is not a CMake listfile"):
            self.fixture.load_corpora()

    def test_single_character_cmake_target_is_rejected(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.inventory["parsers"][0]["target"]["cmake_target"] = "e"
        self.fixture.write_inventory()
        with self.assertPolicyError(r"cmake_target does not match"):
            self.fixture.load()

    # -- corpus integrity --------------------------------------------------
    def test_missing_corpus_for_fuzzed_parser_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"] = []
        self.fixture.write_corpus()
        with self.assertPolicyError("corpus/parser mismatch"):
            self.fixture.load_corpora()

    def test_future_verification_date_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["last_verified"] = (TODAY + timedelta(days=1)).isoformat()
        self.fixture.write_corpus()
        with self.assertPolicyError("last_verified must not be in the future"):
            self.fixture.load_corpora()

    def test_stale_corpus_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["last_verified"] = (TODAY - timedelta(days=91)).isoformat()
        self.fixture.write_corpus()
        with self.assertPolicyError(r"corpus is stale \(91 days"):
            self.fixture.load_corpora()

    def test_changed_seed_invalidates_the_verification_date(self) -> None:
        self.fixture.make_fuzzed()
        (self.root / "Tests" / "fuzz-corpora" / "example" / "nested.json").write_bytes(b'{"a":9}')
        with self.assertPolicyError("corpus content changed since"):
            self.fixture.load_corpora()

    def test_boolean_budget_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["budget"]["max_input_bytes"] = True
        self.fixture.write_corpus()
        with self.assertPolicyError(r"budget\.max_input_bytes must be an integer"):
            self.fixture.load_corpora()

    def test_float_budget_is_rejected_by_json_loader(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["budget"]["max_input_bytes"] = 1.5
        self.fixture.write_corpus()
        with self.assertPolicyError("non-integer"):
            self.fixture.load_corpora()

    def test_empty_seed_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        (self.root / "Tests" / "fuzz-corpora" / "example" / "empty-object.json").write_bytes(b"")
        self.fixture.refresh_digest()
        with self.assertPolicyError("corpus seed is empty"):
            self.fixture.load_corpora()

    def test_hardlinked_seed_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        seed = self.root / "Tests" / "fuzz-corpora" / "example" / "empty-object.json"
        try:
            os.link(seed, seed.with_name("linked.json"))
        except OSError as exc:  # pragma: no cover - filesystem dependent
            self.fail(f"hard-link fixture unavailable on this filesystem: {exc}")
        with self.assertPolicyError("corpus refuses hard link"):
            self.fixture.load_corpora()

    def test_unreadable_seed_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        with mock.patch.object(policy_common.os, "open", side_effect=PermissionError("denied")):
            with self.assertPolicyError("cannot be opened"):
                self.fixture.load_corpora()

    def test_corpus_outside_the_reviewed_root_is_fatal(self) -> None:
        self.fixture.make_fuzzed()
        self.fixture.corpus["corpora"][0]["corpus_dir"] = "src"
        self.fixture.write_corpus()
        with self.assertPolicyError("must live under Tests/fuzz-corpora"):
            self.fixture.load_corpora()

    def test_two_parsers_may_not_share_one_target(self) -> None:
        self.fixture.make_fuzzed()
        clone = json.loads(json.dumps(self.fixture.inventory["parsers"][0]))
        clone["id"] = "example-parser-clone"
        clone["source_files"] = ["src/ExampleParser.h"]
        self.fixture.inventory["parsers"][0]["source_files"] = ["src/ExampleParser.cpp"]
        self.fixture.inventory["parsers"].append(clone)
        self.fixture.write_inventory()
        with self.assertPolicyError(r"reuses target\.cmake_target across parsers"):
            self.fixture.load()


# =========================================================================
# Gate behaviour
# =========================================================================
class TestGateBehavior(FixtureTestCase):
    def test_blockers_make_the_gate_report_not_passed(self) -> None:
        report = check_fuzz_policy.build_check_report(
            self.root, parser_inventory.DEFAULT_INVENTORY, corpus_manifest.DEFAULT_CORPUS_MANIFEST
        )
        self.assertFalse(report["passed"])
        self.assertFalse(report["policy_complete"])
        self.assertFalse(report["coverage_claim"])
        self.assertEqual(
            report["closure_blockers"],
            ["1 inventoried parsers have no fuzz target", "no corpus/resource budget is bound to a runnable fuzz target"],
        )

    def test_require_closure_exits_nonzero_while_blockers_remain(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = check_fuzz_policy.main(["--source-root", str(self.root), "--require-closure"])
        self.assertEqual(result, 1)

    def test_structural_gate_passes_while_blockers_remain(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()):
            result = check_fuzz_policy.main(["--source-root", str(self.root)])
        self.assertEqual(result, 0)

    def test_report_records_input_digests(self) -> None:
        report = check_fuzz_policy.build_check_report(
            self.root, parser_inventory.DEFAULT_INVENTORY, corpus_manifest.DEFAULT_CORPUS_MANIFEST
        )
        raw = (self.root / "tools" / "fuzz-policy" / "parser-inventory.json").read_bytes()
        self.assertEqual(report["inputs"]["inventory"], hashlib.sha256(raw).hexdigest())

    def test_emit_json_failure_returns_nonzero(self) -> None:
        self.fixture.write_inventory("{}")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = check_fuzz_policy.main(["--source-root", str(self.root), "--emit-json"])
        self.assertEqual(result, 1)
        payload = json.loads(output.getvalue())
        self.assertFalse(payload["passed"])
        self.assertFalse(payload["structural_gate"])

    def test_invalid_corpus_emit_json_returns_nonzero(self) -> None:
        self.fixture.write_corpus("{}")
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = corpus_manifest.main(["--source-root", str(self.root), "--emit-json"])
        self.assertEqual(result, 1)
        self.assertFalse(json.loads(output.getvalue())["passed"])

    def test_stale_evidence_is_fatal(self) -> None:
        (self.root / "evidence.json").write_text("{}", encoding="utf-8")
        with self.assertPolicyError("is stale; regenerate it"):
            check_fuzz_policy.validate_evidence(self.root, {"passed": True}, "evidence.json")

    def test_ledger_may_not_out_claim_the_report(self) -> None:
        (self.root / "ledger.json").write_text(
            json.dumps(
                {
                    "status": "closed",
                    "blocking": False,
                    "closure_claim": True,
                    "verified_snapshot": check_fuzz_policy.DEFAULT_EVIDENCE,
                    "not_completed": [],
                }
            ),
            encoding="utf-8",
        )
        report = {"closure_blockers": ["something remains"]}
        with self.assertPolicyError("claims closure while 1 blockers remain"):
            check_fuzz_policy.validate_ledger(self.root, report, "ledger.json")

    def test_ledger_must_stay_open_and_blocking(self) -> None:
        (self.root / "ledger.json").write_text(
            json.dumps(
                {
                    "status": "closed",
                    "blocking": False,
                    "closure_claim": False,
                    "verified_snapshot": check_fuzz_policy.DEFAULT_EVIDENCE,
                    "not_completed": ["x"],
                }
            ),
            encoding="utf-8",
        )
        with self.assertPolicyError("must stay open and blocking"):
            check_fuzz_policy.validate_ledger(self.root, {"closure_blockers": ["x"]}, "ledger.json")


class TestWorkflowBinding(unittest.TestCase):
    """The CI proof must read the workflow, not grep it."""

    def setUp(self) -> None:
        self.workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")

    def test_required_commands_are_seen_as_run_steps(self) -> None:
        block = check_fuzz_policy._job_block(self.workflow, check_fuzz_policy.FUZZ_JOB)
        commands = check_fuzz_policy._run_commands(block)
        for literal in check_fuzz_policy.REQUIRED_JOB_COMMANDS:
            self.assertIn(literal, commands)

    def test_commented_out_command_is_not_a_run_step(self) -> None:
        block = [
            "  fuzz-policy:",
            "    runs-on: ubuntu-24.04",
            "    steps:",
            "    - name: Configure",
            "      # run: cmake -S tools/fuzz-policy -B build/fuzz-policy",
            "      run: true",
        ]
        self.assertNotIn("cmake -S tools/fuzz-policy -B build/fuzz-policy", check_fuzz_policy._run_commands(block))

    def test_echoed_command_is_not_a_run_step(self) -> None:
        block = [
            "  fuzz-policy:",
            "    steps:",
            "    - run: echo 'cmake --build build/fuzz-policy --target check-fuzz-policy'",
        ]
        commands = check_fuzz_policy._run_commands(block)
        self.assertNotIn("cmake --build build/fuzz-policy --target check-fuzz-policy", commands)

    def test_block_scalar_body_is_a_command(self) -> None:
        block = [
            "  fuzz-policy:",
            "    steps:",
            "    - name: Multi",
            "      run: |",
            "        cmake -S tools/fuzz-policy -B build/fuzz-policy",
            "        ctest --test-dir build/fuzz-policy",
        ]
        commands = check_fuzz_policy._run_commands(block)
        self.assertIn("cmake -S tools/fuzz-policy -B build/fuzz-policy", commands)
        self.assertIn("ctest --test-dir build/fuzz-policy", commands)

    def test_sibling_key_after_a_block_scalar_is_not_a_command(self) -> None:
        block = [
            "  fuzz-policy:",
            "    steps:",
            "    - run: |",
            "        echo hello",
            "      name: cmake --build build/fuzz-policy --target check-fuzz-policy",
        ]
        commands = check_fuzz_policy._run_commands(block)
        self.assertNotIn("name: cmake --build build/fuzz-policy --target check-fuzz-policy", commands)
        self.assertNotIn("cmake --build build/fuzz-policy --target check-fuzz-policy", commands)

    def test_disabled_job_is_rejected(self) -> None:
        block = ["  fuzz-policy:", "    if: false", "    runs-on: ubuntu-24.04"]
        with self.assertRaisesRegex(policy_common.PolicyError, "must not be conditional"):
            check_fuzz_policy._assert_job_is_live(block, "fuzz-policy")

    def test_continue_on_error_job_is_rejected(self) -> None:
        block = ["  fuzz-policy:", "    continue-on-error: true", "    runs-on: ubuntu-24.04"]
        with self.assertRaisesRegex(policy_common.PolicyError, "must not set continue-on-error"):
            check_fuzz_policy._assert_job_is_live(block, "fuzz-policy")

    def test_duplicate_job_definition_is_rejected(self) -> None:
        duplicated = self.workflow + "\n  fuzz-policy:\n    runs-on: ubuntu-24.04\n"
        with self.assertRaisesRegex(policy_common.PolicyError, "exactly one fuzz-policy job"):
            check_fuzz_policy._job_block(duplicated, check_fuzz_policy.FUZZ_JOB)


class CiBindingFixture:
    """A copy of the real CI/CMake wiring that individual tests can break."""

    FILES = (
        ".github/workflows/build.yml",
        "CMakeLists.txt",
        "cmake/SparkFuzzPolicy.cmake",
        "tools/fuzz-policy/CMakeLists.txt",
    )

    def __init__(self, root: pathlib.Path) -> None:
        self.root = root
        for relative in self.FILES:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes((REPO_ROOT / relative).read_bytes())

    def patch(self, relative: str, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8")
        assert old in text, f"{relative} does not contain {old!r}"
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def validate(self, fuzz_target_count: int = 0) -> None:
        check_fuzz_policy.validate_ci_and_cmake_binding(self.root, fuzz_target_count=fuzz_target_count)


class TestCiBindingMutations(unittest.TestCase):
    """validate_ci_and_cmake_binding must fail on a broken wiring, not just pass on a good one."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.fixture = CiBindingFixture(pathlib.Path(self.temp.name))

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_unmodified_wiring_passes(self) -> None:
        self.fixture.validate()

    def test_conditional_job_is_rejected(self) -> None:
        self.fixture.patch(".github/workflows/build.yml", '  fuzz-policy:\n    name: "Fuzz policy"', '  fuzz-policy:\n    if: false\n    name: "Fuzz policy"')
        with self.assertRaisesRegex(policy_common.PolicyError, "must not be conditional"):
            self.fixture.validate()

    def test_continue_on_error_job_is_rejected(self) -> None:
        self.fixture.patch(".github/workflows/build.yml", '  fuzz-policy:\n    name: "Fuzz policy"', '  fuzz-policy:\n    continue-on-error: true\n    name: "Fuzz policy"')
        with self.assertRaisesRegex(policy_common.PolicyError, "must not set continue-on-error"):
            self.fixture.validate()

    def test_commented_out_command_is_rejected(self) -> None:
        self.fixture.patch(
            ".github/workflows/build.yml",
            "      run: cmake --build build/fuzz-policy --target check-fuzz-policy",
            "      # run: cmake --build build/fuzz-policy --target check-fuzz-policy\n      run: true",
        )
        with self.assertRaisesRegex(policy_common.PolicyError, "does not run 'cmake --build"):
            self.fixture.validate()

    def test_required_gate_without_the_dependency_is_rejected(self) -> None:
        text = (self.fixture.root / ".github/workflows/build.yml").read_text(encoding="utf-8")
        gate = check_fuzz_policy._job_block(text, "required-ci-gate")
        stripped = "\n".join(line for line in gate if line.strip() != "- fuzz-policy")
        (self.fixture.root / ".github/workflows/build.yml").write_text(
            text.replace("\n".join(gate), stripped, 1), encoding="utf-8"
        )
        with self.assertRaisesRegex(policy_common.PolicyError, "does not depend on fuzz-policy"):
            self.fixture.validate()

    def test_root_cmake_without_the_include_is_rejected(self) -> None:
        self.fixture.patch(
            "CMakeLists.txt",
            'include("${CMAKE_SOURCE_DIR}/cmake/SparkFuzzPolicy.cmake")',
            'include("${CMAKE_SOURCE_DIR}/cmake/SomethingElse.cmake")',
        )
        with self.assertRaisesRegex(policy_common.PolicyError, "does not include cmake/SparkFuzzPolicy.cmake"):
            self.fixture.validate()

    def test_module_without_the_python_option_gate_is_rejected(self) -> None:
        self.fixture.patch("cmake/SparkFuzzPolicy.cmake", "option(SPARK_ENABLE_FUZZ_POLICY_CHECKS", "set(SPARK_ENABLE_FUZZ_POLICY_CHECKS")
        with self.assertRaisesRegex(policy_common.PolicyError, "does not gate its Python dependency"):
            self.fixture.validate()

    def test_module_without_the_adversarial_test_is_rejected(self) -> None:
        self.fixture.patch("cmake/SparkFuzzPolicy.cmake", "NAME FuzzPolicyAdversarial", "NAME FuzzPolicyRenamed")
        with self.assertRaisesRegex(policy_common.PolicyError, "does not register the FuzzPolicyAdversarial test"):
            self.fixture.validate()

    def test_standalone_project_without_enable_testing_is_rejected(self) -> None:
        self.fixture.patch("tools/fuzz-policy/CMakeLists.txt", "enable_testing()", "# enable_testing()")
        with self.assertRaisesRegex(policy_common.PolicyError, "is missing enable_testing"):
            self.fixture.validate()


# =========================================================================
# Repository integration
# =========================================================================
class TestRepositoryIntegration(unittest.TestCase):
    def test_confined_read_canonicalizes_a_non_canonical_root(self) -> None:
        # Built on the repo's own volume: os.path.relpath across drives raises
        # ValueError on Windows, which would turn a portability quirk into a
        # policy failure.
        alias = REPO_ROOT / "tools" / ".."
        payload = policy_common.read_confined_file(alias, "CMakeLists.txt", "root cmake", max_bytes=4 * 1024 * 1024)
        self.assertIn(b"cmake_minimum_required", payload)

    def test_repository_policy_report_is_structurally_valid_but_open(self) -> None:
        report = check_fuzz_policy.build_check_report(
            REPO_ROOT, parser_inventory.DEFAULT_INVENTORY, corpus_manifest.DEFAULT_CORPUS_MANIFEST
        )
        self.assertTrue(report["structural_gate"])
        self.assertFalse(report["passed"])
        self.assertFalse(report["policy_complete"])
        self.assertEqual(report["inventory"]["unclassified_candidate_count"], 0)
        self.assertGreater(report["inventory"]["parser_count"], 100)
        self.assertGreater(report["inventory"]["scanned_file_count"], 1000)

    def test_named_verified_misses_are_inventoried(self) -> None:
        inventory = parser_inventory.load_inventory(REPO_ROOT)
        owned = {source for parser in inventory.parsers for source in parser.source_files}
        for path in (
            "SparkEngine/Source/Core/ModuleManager.cpp",
            "SparkLauncher/src/LauncherProcess.cpp",
            "SparkLauncher/src/LauncherApp.cpp",
            "SparkEngine/Source/Utils/TelemetrySpoolFormat.cpp",
            "SparkEditor/Source/Panels/RegionMapDataSource.cpp",
            "SparkEditor/Source/AssetPipeline/AdvancedAssetPipelineUI.cpp",
            "SparkEngine/Source/Engine/Replay/ReplaySystem.cpp",
            "GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabase.cpp",
            "GameModules/SparkGameMMOFPS/Source/Persistence/TFOutfitStoreDisk.cpp",
            "SparkEngine/Source/Graphics/Neural/NeuralWeights.cpp",
        ):
            self.assertIn(path, owned, f"{path} is not inventoried")

    def test_service_roots_are_scanned(self) -> None:
        roots = set(parser_inventory.load_inventory(REPO_ROOT).scope.roots)
        for root in ("SparkDaemon/src", "SparkServer/src", "SparkGateway/src"):
            self.assertIn(root, roots)

    def test_every_deferral_names_an_owner_and_expiry(self) -> None:
        inventory = parser_inventory.load_inventory(REPO_ROOT)
        self.assertGreater(len(inventory.deferred_candidates), 0)
        for deferral in inventory.deferred_candidates:
            self.assertEqual(deferral.owner, parser_inventory.DEFERRAL_OWNER)
            self.assertEqual(deferral.ticket, parser_inventory.DEFERRAL_TICKET)
            self.assertGreater(deferral.expires, TODAY)
            self.assertGreater(len(deferral.reason), 20)

    def test_ci_and_cmake_are_blocking_wired(self) -> None:
        check_fuzz_policy.validate_ci_and_cmake_binding(REPO_ROOT, fuzz_target_count=0)

    def test_ci_binding_rejects_a_missing_required_gate_dependency(self) -> None:
        workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        gate = check_fuzz_policy._job_block(workflow, "required-ci-gate")
        self.assertTrue(any(line.strip() == "- fuzz-policy" for line in gate))

    def test_fuzz_targets_would_require_a_smoke_run(self) -> None:
        # No fuzz target exists yet, so the smoke command is intentionally absent.
        # Declaring one without wiring the run must fail.
        with self.assertRaisesRegex(policy_common.PolicyError, "never runs"):
            check_fuzz_policy.validate_ci_and_cmake_binding(REPO_ROOT, fuzz_target_count=1)

    def test_ctest_entries_run_from_the_source_root(self) -> None:
        module = (REPO_ROOT / "cmake" / "SparkFuzzPolicy.cmake").read_text(encoding="utf-8")
        self.assertEqual(module.count('WORKING_DIRECTORY "${source_root}"'), 3)

    def test_committed_paths_use_canonical_case(self) -> None:
        # A pathspec is laundered to the caller's case by git on a
        # case-insensitive filesystem, so enumerate everything and filter here.
        tracked = subprocess.check_output(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
            cwd=REPO_ROOT,
            text=True,
            encoding="utf-8",
        ).splitlines()
        policy_paths = [path for path in tracked if path.lower().startswith(("tools/fuzz-policy/", "tests/fuzz-policy/"))]
        self.assertIn("tools/fuzz-policy/check_fuzz_policy.py", policy_paths)
        miscased = [path for path in policy_paths if not path.startswith(("tools/fuzz-policy/", "Tests/fuzz-policy/"))]
        self.assertEqual(miscased, [], f"policy files recorded under a non-canonical directory case: {miscased}")

    def test_suite_discovers_a_meaningful_number_of_tests(self) -> None:
        # A discovery run that finds nothing exits 0 on Python <= 3.11, so pin a
        # floor here as well as in the CMake registration.
        loader = unittest.TestLoader()
        discovered = loader.discover(str(REPO_ROOT / "Tests" / "fuzz-policy"), pattern="test_*.py")
        self.assertGreaterEqual(discovered.countTestCases(), 100)


if __name__ == "__main__":
    unittest.main()
