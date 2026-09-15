#!/usr/bin/env python3
"""Adversarial tests for the CI-110 flaky-waiver policy validator."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPO_ROOT / "Tools" / "validate_test_warnings.py"
AS_OF = "2026-09-13"


def registry(*patterns: str) -> str:
    entries = ",\n".join(f'    {{"{pattern}", "known reason"}}' for pattern in patterns)
    return (
        "inline constexpr TestWarningPattern g_testWarningPatterns[] = {\n"
        f"{entries}\n"
        "};\n"
    )


def metadata(*entries: dict[str, str]) -> str:
    return json.dumps({"schemaVersion": 1, "waivers": list(entries)}) + "\n"


def waiver(pattern: str, owner: str = "engine-qa", expires: str = "2026-09-14") -> dict[str, str]:
    return {"pattern": pattern, "owner": owner, "expires": expires}


class TestWarningPolicy(unittest.TestCase):
    def run_validator(self, registry_text: str, metadata_text: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            registry_path = root / "TestWarnings.h"
            metadata_path = root / "test-warning-waivers.json"
            registry_path.write_text(registry_text, encoding="utf-8")
            metadata_path.write_text(metadata_text, encoding="utf-8")
            return subprocess.run(
                [
                    sys.executable,
                    str(VALIDATOR),
                    "--registry",
                    str(registry_path),
                    "--metadata",
                    str(metadata_path),
                    "--as-of",
                    AS_OF,
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
            )

    def test_accepts_exactly_owned_future_waivers(self) -> None:
        result = self.run_validator(
            registry("Flaky_A", "Flaky_B"),
            metadata(waiver("Flaky_A"), waiver("Flaky_B", owner="networking")),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("validated 2 flaky waiver", result.stdout)

    def test_rejects_registry_pattern_without_metadata(self) -> None:
        result = self.run_validator(
            registry("Flaky_A", "Flaky_B"),
            metadata(waiver("Flaky_A")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing metadata", result.stderr)

    def test_rejects_metadata_pattern_without_registry_entry(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"),
            metadata(waiver("Flaky_A"), waiver("Stale_Entry")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not present in TestWarnings.h", result.stderr)

    def test_rejects_unowned_placeholder(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"),
            metadata(waiver("Flaky_A", owner="unassigned")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("owner", result.stderr)

    def test_rejects_expired_waiver_at_as_of_date(self) -> None:
        result = self.run_validator(
            registry("Flaky_A"),
            metadata(waiver("Flaky_A", expires=AS_OF)),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("expired", result.stderr)

    def test_rejects_duplicate_registry_patterns(self) -> None:
        result = self.run_validator(
            registry("Flaky_A", "Flaky_A"),
            metadata(waiver("Flaky_A")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate pattern", result.stderr)

    def test_rejects_extra_metadata_fields(self) -> None:
        entry = waiver("Flaky_A")
        entry["reason"] = "duplicated source-of-truth"
        result = self.run_validator(registry("Flaky_A"), metadata(entry))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly", result.stderr)


if __name__ == "__main__":
    unittest.main()
