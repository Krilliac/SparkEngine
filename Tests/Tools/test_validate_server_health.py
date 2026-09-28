#!/usr/bin/env python3
"""OPS-110: the SparkServer health snapshot's external contract (spark-server-health/1).

tools/ops/validate_server_health.py is the one parser outside consumers use.
These tests prove it accepts the snapshot SparkServer publishes and refuses
every way a snapshot can drift from the documented contract: a missing,
unknown or duplicate key, a wrong type (including a boolean posing as an
integer), a short or uppercase commit, an unknown tree state, unordered tick
percentiles, a queue peak below its depth, and an oversized document. With
an expected SHA it also refuses an unstamped commit, a dirty tree, and any
other commit. RunbookContractTests keep the runbook's field table equal to
the validator's key set, so the documentation cannot drift either.
"""

from __future__ import annotations

import io
import json
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "ops"))

import validate_server_health as contract  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"
OTHER_SHA = "fedcba9876543210fedcba9876543210fedcba98"
RUNBOOK = REPO_ROOT / "wiki" / "advanced" / "Server-Operations-Runbook.md"
HEALTH_SOURCE = REPO_ROOT / "SparkServer" / "src" / "ServerHealth.cpp"


def snapshot(**overrides: object) -> dict:
    """A snapshot exactly as SparkServer's FormatHealthJson emits it."""
    record = {"schema": contract.SCHEMA, "live": True, "ready": True, "draining": False, "stopping": False,
              "port": 27015, "players": 2, "ticks": 600, "loadedModules": 1, "gameModule": "SparkGame",
              "map": "soak", "error": "", "version": "1.2.3", "commit": SHA, "treeState": "clean",
              "tickSamples": 600, "tickP50Us": 100, "tickP95Us": 2000, "tickP99Us": 4000, "tickMaxUs": 5000,
              "rssBytes": 123456789, "netQueueIn": 0, "netQueueOut": 3, "netQueueInPeak": 4, "netQueueOutPeak": 40}
    record.update(overrides)
    return record


def encode(record: dict) -> bytes:
    return json.dumps(record, separators=(",", ":")).encode("utf-8")


class ParseTests(unittest.TestCase):
    def assertViolation(self, data: bytes, fragment: str) -> None:
        with self.assertRaises(contract.HealthContractError) as caught:
            contract.parse_health(data)
        self.assertIn(fragment, str(caught.exception))

    def test_published_snapshot_is_accepted(self) -> None:
        self.assertEqual(contract.parse_health(encode(snapshot()) + b"\n"), snapshot())
        self.assertIsNone(contract.parse_health(encode(snapshot(rssBytes=None)))["rssBytes"])
        self.assertEqual(contract.parse_health(encode(snapshot(commit="unknown", treeState="unknown")))["commit"],
                         "unknown")

    def test_schema_must_be_the_versioned_identifier(self) -> None:
        self.assertViolation(encode(snapshot(schema="spark-server-health/2")), "schema is")
        record = snapshot()
        del record["schema"]
        self.assertViolation(encode(record), "schema is None")

    def test_key_set_is_exact(self) -> None:
        record = snapshot()
        del record["netQueueOutPeak"]
        self.assertViolation(encode(record), "missing ['netQueueOutPeak']")
        self.assertViolation(encode(snapshot(extra=1)), "unknown ['extra']")

    def test_duplicate_keys_are_refused(self) -> None:
        text = encode(snapshot()).decode("utf-8")
        self.assertViolation(text.replace('"players":2', '"players":2,"players":3').encode("utf-8"),
                             "duplicate JSON object key 'players'")

    def test_wrong_types_are_refused(self) -> None:
        cases = {"live": 1, "ticks": True, "players": -1, "tickP99Us": 1.5, "gameModule": None, "port": 70000,
                 "rssBytes": "12", "netQueueOut": 2**64, "error": 0}
        for key, value in cases.items():
            with self.subTest(key=key):
                self.assertViolation(encode(snapshot(**{key: value})), key)

    def test_commit_must_be_full_lowercase_hex_or_unknown(self) -> None:
        for commit in (SHA[:12], SHA.upper(), SHA + "0", "", "UNKNOWN", 12345):
            with self.subTest(commit=commit):
                self.assertViolation(encode(snapshot(commit=commit)), "commit must be")

    def test_tree_state_is_an_enumeration(self) -> None:
        self.assertViolation(encode(snapshot(treeState="modified")), "treeState must be")

    def test_tick_percentiles_are_ordered(self) -> None:
        self.assertViolation(encode(snapshot(tickP95Us=5000, tickP99Us=4000)), "not ordered")
        self.assertViolation(encode(snapshot(tickMaxUs=10)), "not ordered")

    def test_queue_peak_never_trails_its_depth(self) -> None:
        self.assertViolation(encode(snapshot(netQueueOut=41)), "netQueueOutPeak 40 is below the current netQueueOut")
        self.assertViolation(encode(snapshot(netQueueIn=5)), "netQueueInPeak")

    def test_malformed_and_oversized_documents_are_refused(self) -> None:
        self.assertViolation(b"[1, 2]", "not a JSON object")
        self.assertViolation(b'{"live": NaN}', "non-finite")
        self.assertViolation(b"\xff\xfe", "not valid UTF-8")
        self.assertViolation(encode(snapshot(error="x" * (contract.MAX_HEALTH_BYTES + 1))), "exceeds")
        self.assertViolation(encode(snapshot(error={"nested": {"deep": 1}})), "depth")


class IdentityTests(unittest.TestCase):
    def test_no_expected_sha_accepts_any_valid_identity(self) -> None:
        self.assertIsNone(contract.build_identity_problem(snapshot(commit="unknown", treeState="dirty"), None))

    def test_exact_clean_commit_is_accepted_case_insensitively(self) -> None:
        self.assertIsNone(contract.build_identity_problem(snapshot(), SHA))
        self.assertIsNone(contract.build_identity_problem(snapshot(), SHA.upper()))

    def test_exact_sha_refusals(self) -> None:
        cases = {"unstamped": (snapshot(commit="unknown"), "commit 'unknown'"),
                 "mismatch": (snapshot(commit=OTHER_SHA), "expected"),
                 "dirty": (snapshot(treeState="dirty"), "'dirty' tree"),
                 "unknown-tree": (snapshot(treeState="unknown"), "'unknown' tree")}
        for name, (health, fragment) in cases.items():
            with self.subTest(name=name):
                problem = contract.build_identity_problem(health, SHA)
                self.assertIsNotNone(problem)
                self.assertIn(fragment, problem)
        self.assertIn("not a full", contract.build_identity_problem(snapshot(), SHA[:7]))


class CliTests(unittest.TestCase):
    def run_cli(self, data: bytes, *extra: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "health.json"
            path.write_bytes(data)
            stdout, stderr = io.StringIO(), io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                status = contract.main([str(path), *extra])
        return status, stdout.getvalue() + stderr.getvalue()

    def test_valid_snapshot_passes_with_and_without_expected_sha(self) -> None:
        self.assertEqual(self.run_cli(encode(snapshot()) + b"\n")[0], 0)
        status, output = self.run_cli(encode(snapshot()), "--expected-sha", SHA)
        self.assertEqual(status, 0, output)
        self.assertIn(SHA, output)

    def test_violations_and_identity_refusals_exit_nonzero(self) -> None:
        self.assertEqual(self.run_cli(encode(snapshot(treeState="dirty")), "--expected-sha", SHA)[0], 1)
        self.assertEqual(self.run_cli(encode(snapshot(commit="unknown")), "--expected-sha", SHA)[0], 1)
        self.assertEqual(self.run_cli(encode(snapshot(schema="x")))[0], 1)
        self.assertEqual(self.run_cli(b"")[0], 1)

    def test_missing_file_exits_nonzero(self) -> None:
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            self.assertEqual(contract.main([str(REPO_ROOT / "does-not-exist.json")]), 1)
        self.assertIn("cannot read", stderr.getvalue())


class RunbookContractTests(unittest.TestCase):
    """The runbook's field table and the C++ serializer both name exactly the validator's key set."""

    def test_runbook_documents_every_field(self) -> None:
        text = RUNBOOK.read_text(encoding="utf-8")
        self.assertIn(contract.SCHEMA, text)
        section = text[text.index("## Health Snapshot Contract"):]
        section = section[:section.index("\n## ", 1)]
        documented = set(re.findall(r"^\| `([A-Za-z0-9]+)` \|", section, re.MULTILINE))
        self.assertEqual(documented, set(contract.FIELDS))

    def test_serializer_emits_every_field(self) -> None:
        source = HEALTH_SOURCE.read_text(encoding="utf-8")
        body = source[source.index("std::string FormatHealthJson"):source.index("void WriteHealthFile")]
        emitted = set(re.findall(r'\\"([A-Za-z0-9]+)\\":', body))
        self.assertEqual(emitted, set(contract.FIELDS))


if __name__ == "__main__":
    unittest.main()
