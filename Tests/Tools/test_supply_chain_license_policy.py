#!/usr/bin/env python3
"""SEC-110: SPDX license allow-list enforcement in tools/check-supply-chain.py.

Every third-party dependency in ThirdParty/dependencies.lock must resolve to an
SPDX license expression whose every license identifier is on the checker's
allow-list.  The resolution is either the manifest's own license field (when
it is already a well-formed SPDX expression) or a reviewed mapping recorded in
ThirdParty/supply-chain.lock under ``license_policy.dependencies``.  A mapping
pins the exact declared manifest string, so editing the manifest's license
text forces the SPDX mapping to be re-reviewed instead of silently inheriting
the old verdict.

Negative cases reuse the fake-repository fixture from
Tests/test_check_supply_chain.py: start from a tree the real checker passes,
change exactly one thing, and require a failure.

Run:  python Tests/Tools/test_supply_chain_license_policy.py
      python -m pytest Tests/Tools/test_supply_chain_license_policy.py -v
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import unittest
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
BASE_TEST_REL = "Tests/test_check_supply_chain.py"

_spec = importlib.util.spec_from_file_location(
    "spark_supply_chain_base_tests", str(PROJECT_ROOT / BASE_TEST_REL),
)
base = importlib.util.module_from_spec(_spec)
sys.modules["spark_supply_chain_base_tests"] = base
_spec.loader.exec_module(base)
sc = base.sc

DEMO_FIELDS = list(base.TestReconciliation.BASE_FIELDS)
FIELD_NAMES = [
    "name", "source", "version", "license", "local_path",
    "required", "macro", "fallback", "severity", "notices",
]


def demo_entry(**overrides: str) -> str:
    fields = list(DEMO_FIELDS)
    for key, value in overrides.items():
        fields[FIELD_NAMES.index(key)] = value
    return "|".join(fields)


# ═══════════════════════════════════════════════════════════════════════
# SPDX expression parser (pure unit tests, no fixture repository)
# ═══════════════════════════════════════════════════════════════════════

class TestSpdxExpressionParser(unittest.TestCase):

    def test_single_identifier(self) -> None:
        self.assertEqual(sc.parse_spdx_expression("MIT"), ["MIT"])

    def test_disjunction_returns_every_leaf(self) -> None:
        self.assertEqual(
            sc.parse_spdx_expression("MIT OR Unlicense"), ["MIT", "Unlicense"],
        )

    def test_parenthesised_conjunction(self) -> None:
        self.assertEqual(
            sc.parse_spdx_expression("(MIT AND Apache-2.0) OR Zlib"),
            ["MIT", "Apache-2.0", "Zlib"],
        )

    def test_rejects_prose(self) -> None:
        for prose in (
            "Public Domain / MIT",
            "MIT and Apache-2.0",
            "BSD-3-Clause (library, upstream tools are dual-licensed)",
            "",
            "   ",
        ):
            with self.subTest(prose=prose):
                with self.assertRaises(ValueError):
                    sc.parse_spdx_expression(prose)

    def test_rejects_dangling_operator(self) -> None:
        for bad in ("MIT OR", "AND MIT", "MIT OR OR Zlib", "(MIT", "MIT)", "()"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    sc.parse_spdx_expression(bad)

    def test_rejects_juxtaposed_identifiers(self) -> None:
        with self.assertRaises(ValueError):
            sc.parse_spdx_expression("MIT Apache-2.0")

    def test_with_exception_is_not_silently_accepted(self) -> None:
        with self.assertRaises(ValueError):
            sc.parse_spdx_expression("Apache-2.0 WITH LLVM-exception")

    def test_expression_length_is_bounded(self) -> None:
        with self.assertRaises(ValueError):
            sc.parse_spdx_expression(" OR ".join(["MIT"] * 400))

    def test_allow_list_is_the_audited_permissive_set(self) -> None:
        # Widening the allow-list is a policy decision that must show up as a
        # reviewed diff to this assertion, not as a quiet constant edit.
        self.assertEqual(
            sorted(sc.ALLOWED_SPDX_LICENSES),
            ["Apache-2.0", "BSD-3-Clause", "MIT", "MIT-0", "Unlicense", "Zlib"],
        )


# ═══════════════════════════════════════════════════════════════════════
# Fixture repository: the checker enforces the allow-list end to end
# ═══════════════════════════════════════════════════════════════════════

class LicensePolicyCase(base.FakeRepoCase):

    def set_manifest(self, *entries: str) -> None:
        body = "".join(f'    "{entry}"\n' for entry in entries)
        self.write(
            "ThirdParty/dependencies.lock",
            f"set(SPARK_THIRDPARTY_AUDIT_ENTRIES\n{body})\n",
        )

    def set_policy(self, dependencies: dict[str, dict[str, str]]) -> None:
        data = self.lock()
        data["license_policy"] = {"dependencies": dependencies}
        self.set_lock(data)

    def licenses(self) -> dict[str, str]:
        done = self.check("--json")
        payload = json.loads(done.stdout)
        return {
            item["name"]: item["spdx"] for item in payload["dependency_licenses"]
        }


class TestLicenseAllowList(LicensePolicyCase):

    def test_baseline_spdx_manifest_license_passes_and_is_reported(self) -> None:
        self.assert_baseline_passes()
        self.assertEqual(self.licenses(), {"demo": "MIT"})

    def test_disallowed_spdx_license_is_a_violation(self) -> None:
        self.set_manifest(demo_entry(license="GPL-3.0-only"))
        self.commit()
        self.assert_violation("GPL-3.0-only", "not on the license allow-list")

    def test_prose_license_without_mapping_is_a_violation(self) -> None:
        self.set_manifest(demo_entry(license="Public Domain / MIT"))
        self.commit()
        self.assert_violation("is not an SPDX expression", "license_policy")

    def test_case_variant_identifier_is_not_normalised(self) -> None:
        self.set_manifest(demo_entry(license="mit"))
        self.commit()
        self.assert_violation("'mit'", "not on the license allow-list")

    def test_license_ref_is_a_violation(self) -> None:
        self.set_manifest(demo_entry(license="LicenseRef-Proprietary"))
        self.commit()
        self.assert_violation("LicenseRef-Proprietary", "not on the license allow-list")

    def test_disjunction_with_one_disallowed_leaf_is_a_violation(self) -> None:
        # Fail closed: every identifier named must be approved, so an
        # expression cannot smuggle an unreviewed license behind an OR.
        self.set_manifest(demo_entry(license="MIT OR GPL-2.0-only"))
        self.commit()
        self.assert_violation("GPL-2.0-only", "not on the license allow-list")

    def test_with_exception_expression_is_a_violation(self) -> None:
        self.set_manifest(demo_entry(license="Apache-2.0 WITH LLVM-exception"))
        self.commit()
        self.assert_violation("is not an SPDX expression")


class TestLicensePolicyMapping(LicensePolicyCase):

    def test_reviewed_mapping_resolves_prose_declaration(self) -> None:
        self.set_manifest(demo_entry(license="Public Domain / MIT"))
        self.set_policy({
            "demo": {"declared": "Public Domain / MIT", "spdx": "MIT OR Unlicense"},
        })
        self.commit()
        self.assert_baseline_passes()
        self.assertEqual(self.licenses(), {"demo": "MIT OR Unlicense"})

    def test_mapping_overrides_even_a_valid_spdx_declaration(self) -> None:
        self.set_policy({"demo": {"declared": "MIT", "spdx": "MIT OR Unlicense"}})
        self.commit()
        self.assert_baseline_passes()
        self.assertEqual(self.licenses(), {"demo": "MIT OR Unlicense"})

    def test_declared_license_drift_invalidates_the_mapping(self) -> None:
        self.set_manifest(demo_entry(license="Public Domain / MIT-0"))
        self.set_policy({
            "demo": {"declared": "Public Domain / MIT", "spdx": "MIT OR Unlicense"},
        })
        self.commit()
        self.assert_violation("declared license changed", "Public Domain / MIT-0")

    def test_mapping_to_disallowed_license_is_a_violation(self) -> None:
        self.set_manifest(demo_entry(license="Custom"))
        self.set_policy({"demo": {"declared": "Custom", "spdx": "SSPL-1.0"}})
        self.commit()
        self.assert_violation("SSPL-1.0", "not on the license allow-list")

    def test_malformed_mapping_expression_is_a_violation(self) -> None:
        self.set_policy({"demo": {"declared": "MIT", "spdx": "MIT OR"}})
        self.commit()
        self.assert_violation("license_policy", "is not an SPDX expression")

    def test_stale_mapping_for_unknown_dependency_is_a_violation(self) -> None:
        self.set_policy({
            "demo": {"declared": "MIT", "spdx": "MIT"},
            "ghost": {"declared": "MIT", "spdx": "MIT"},
        })
        self.commit()
        self.assert_violation("ghost", "no dependencies.lock entry")

    def test_unknown_mapping_field_is_a_schema_failure(self) -> None:
        self.set_policy({
            "demo": {"declared": "MIT", "spdx": "MIT", "approved_by": "me"},
        })
        self.commit()
        self.assert_fatal("license_policy")

    def test_unknown_policy_key_is_a_schema_failure(self) -> None:
        data = self.lock()
        data["license_policy"] = {"dependencies": {}, "allowed_spdx": ["GPL-3.0-only"]}
        self.set_lock(data)
        self.commit()
        # The allow-list lives in the checker, not in the data it checks.
        self.assert_fatal("license_policy")

    def test_non_object_policy_is_a_schema_failure(self) -> None:
        data = self.lock()
        data["license_policy"] = ["MIT"]
        self.set_lock(data)
        self.commit()
        self.assert_fatal("license_policy")

    def test_update_preserves_the_reviewed_mapping(self) -> None:
        self.set_manifest(demo_entry(license="Public Domain / MIT"))
        policy = {
            "demo": {"declared": "Public Domain / MIT", "spdx": "MIT OR Unlicense"},
        }
        self.set_policy(policy)
        self.commit()
        done = self.check("--update")
        self.assertEqual(done.returncode, 0, f"{done.stdout}\n{done.stderr}")
        self.assertEqual(self.lock()["license_policy"], {"dependencies": policy})


class TestDeterministicOutput(LicensePolicyCase):

    def test_json_output_is_byte_identical_across_runs(self) -> None:
        first = self.check("--json")
        second = self.check("--json")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)

    def test_failing_json_output_is_byte_identical_across_runs(self) -> None:
        self.set_manifest(demo_entry(license="GPL-3.0-only"))
        self.write("ThirdParty/Utils/demo/rogue.h", "/* unlisted */\n")
        self.commit()
        first = self.check("--json")
        second = self.check("--json")
        self.assertEqual(first.returncode, 1, first.stderr)
        self.assertEqual(first.stdout, second.stdout)


# ═══════════════════════════════════════════════════════════════════════
# The real repository: every shipped dependency resolves to an allowed license
# ═══════════════════════════════════════════════════════════════════════

class TestRealRepositoryLicenses(unittest.TestCase):

    @classmethod
    def setUpClass(cls) -> None:
        base._require("git")
        base._require("cmake")
        base._require_yaml()
        cls.done = subprocess.run(
            [sys.executable, base.CHECKER_REL, "--json"],
            cwd=str(PROJECT_ROOT), capture_output=True, text=True, timeout=600,
        )

    def test_real_checker_passes(self) -> None:
        self.assertEqual(
            self.done.returncode, 0, f"{self.done.stdout}\n{self.done.stderr}",
        )

    def test_every_manifest_dependency_has_an_allowed_spdx_license(self) -> None:
        payload = json.loads(self.done.stdout)
        licenses = payload["dependency_licenses"]
        root = PROJECT_ROOT.resolve()
        manifest_names = sorted(
            fields[sc.F_NAME].strip()
            for fields in sc.export_manifest_entries(root, root)
        )
        self.assertGreater(len(manifest_names), 0)
        self.assertEqual(sorted(item["name"] for item in licenses), manifest_names)
        for item in licenses:
            with self.subTest(dependency=item["name"]):
                leaves = sc.parse_spdx_expression(item["spdx"])
                self.assertTrue(set(leaves) <= sc.ALLOWED_SPDX_LICENSES, leaves)

    def test_dependency_licenses_are_sorted_by_name(self) -> None:
        payload = json.loads(self.done.stdout)
        names = [item["name"] for item in payload["dependency_licenses"]]
        self.assertEqual(names, sorted(names))


if __name__ == "__main__":
    unittest.main(verbosity=2)
