"""REL-190: tools/release_qualification.py qualifies a candidate and records why."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_qualification as rq  # noqa: E402
import contract_selectors  # noqa: E402
import exact_evidence  # noqa: E402
from common import load_contract  # noqa: E402
from test_release_stages import candidate  # noqa: E402

REPOSITORY = "Krilliac/SparkEngine"
CANDIDATE = "1" * 40
BLOCKING = frozenset({"required-build"})

# One verify-exact-required-gate.py record that exact_evidence accepts.
EXACT_VALUES = {
    "EXACT_BUILD_RUN_ID": "101",
    "EXACT_BUILD_RUN_ATTEMPT": "2",
    "EXACT_BUILD_RUN_URL": f"https://github.com/{REPOSITORY}/actions/runs/101",
    "EXACT_BUILD_EVENT": "workflow_dispatch",
    "EXACT_BUILD_MATRIX_PRODUCER_JOB_ID": "401",
    "EXACT_BUILD_REQUIRED_GATE_JOB_ID": "402",
    "EXACT_BUILD_JOB_INVENTORY_DIGEST": "sha256:" + "5" * 64,
    "EXACT_BUILD_MATRIX_SOURCE_ARTIFACT_ID": "204",
    "EXACT_BUILD_MATRIX_SOURCE_ARTIFACT_DIGEST": "sha256:" + "6" * 64,
    "EXACT_BUILD_MATRIX_SOURCE_ARTIFACT_BYTES": "4096",
    "EXACT_BUILD_MATRIX_STATUS_ID": "201",
    "EXACT_BUILD_MATRIX_STATUS_TARGET_URL": f"https://github.com/{REPOSITORY}/actions/runs/202/attempts/3",
    "EXACT_BUILD_MATRIX_STATUS_CREATED_AT": "2026-08-30T01:00:02Z",
    "EXACT_BUILD_MATRIX_STATUS_UPDATED_AT": "2026-08-30T01:00:03Z",
    "EXACT_BUILD_MATRIX_VERIFIER_RUN_ID": "202",
    "EXACT_BUILD_MATRIX_VERIFIER_RUN_ATTEMPT": "3",
    "EXACT_BUILD_MATRIX_VERIFIER_RUN_URL": f"https://github.com/{REPOSITORY}/actions/runs/202",
    "EXACT_VERIFIER_COMMIT": "2" * 40,
    "EXACT_BUILD_MATRIX_TRUSTED_VERIFIER_JOB_ID": "403",
    "EXACT_BUILD_MATRIX_VERIFIER_JOB_INVENTORY_DIGEST": "sha256:" + "7" * 64,
    "EXACT_BUILD_MATRIX_STATUS_PUBLISH_STEP_STARTED_AT": "2026-08-30T01:00:00Z",
    "EXACT_BUILD_MATRIX_STATUS_PUBLISH_STEP_COMPLETED_AT": "2026-08-30T01:00:05Z",
    "EXACT_BUILD_MATRIX_RECEIPT_ARTIFACT_ID": "203",
    "EXACT_BUILD_MATRIX_RECEIPT_ARTIFACT_DIGEST": "sha256:" + "3" * 64,
    "EXACT_BUILD_MATRIX_RECEIPT_ARTIFACT_BYTES": "1024",
    "EXACT_CODEQL_RUN_ID": "301",
    "EXACT_CODEQL_RUN_ATTEMPT": "4",
    "EXACT_CODEQL_RUN_URL": f"https://github.com/{REPOSITORY}/actions/runs/301",
    "EXACT_CODEQL_ACTIONS_SOURCE_JOB_ID": "404",
    "EXACT_CODEQL_C_CPP_SOURCE_JOB_ID": "405",
    "EXACT_CODEQL_PYTHON_SOURCE_JOB_ID": "406",
    "EXACT_CODEQL_SOURCE_JOB_INVENTORY_DIGEST": "sha256:" + "8" * 64,
    "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_ID": "305",
    "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_DIGEST": "sha256:" + "9" * 64,
    "EXACT_CODEQL_ACTIONS_SOURCE_ARTIFACT_BYTES": "2048",
    "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_ID": "306",
    "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_DIGEST": "sha256:" + "a" * 64,
    "EXACT_CODEQL_C_CPP_SOURCE_ARTIFACT_BYTES": "3072",
    "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_ID": "307",
    "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_DIGEST": "sha256:" + "b" * 64,
    "EXACT_CODEQL_PYTHON_SOURCE_ARTIFACT_BYTES": "4096",
    "EXACT_CODEQL_STATUS_ID": "302",
    "EXACT_CODEQL_STATUS_TARGET_URL": f"https://github.com/{REPOSITORY}/actions/runs/303/attempts/5",
    "EXACT_CODEQL_STATUS_CREATED_AT": "2026-08-30T02:00:02Z",
    "EXACT_CODEQL_STATUS_UPDATED_AT": "2026-08-30T02:00:03Z",
    "EXACT_CODEQL_REPORTER_RUN_ID": "303",
    "EXACT_CODEQL_REPORTER_RUN_ATTEMPT": "5",
    "EXACT_CODEQL_REPORTER_RUN_URL": f"https://github.com/{REPOSITORY}/actions/runs/303",
    "EXACT_CODEQL_TRUSTED_REPORTER_JOB_ID": "407",
    "EXACT_CODEQL_REPORTER_JOB_INVENTORY_DIGEST": "sha256:" + "c" * 64,
    "EXACT_CODEQL_STATUS_PUBLISH_STEP_STARTED_AT": "2026-08-30T02:00:00Z",
    "EXACT_CODEQL_STATUS_PUBLISH_STEP_COMPLETED_AT": "2026-08-30T02:00:05Z",
    "EXACT_CODEQL_SUMMARY_ARTIFACT_ID": "304",
    "EXACT_CODEQL_SUMMARY_ARTIFACT_DIGEST": "sha256:" + "4" * 64,
    "EXACT_CODEQL_SUMMARY_ARTIFACT_BYTES": "512",
}
GATE_KEY_FROM_ENV = {environment: key for key, environment in exact_evidence.ENV_FROM_GATE_KEY.items()}


def gate_lines() -> list[str]:
    return [f"{GATE_KEY_FROM_ENV[name]}={value}" for name, value in EXACT_VALUES.items()]


def qualifying_contract() -> dict:
    """test_release_stages' candidate, with its profile named for the stage it qualifies."""
    contract = candidate()
    contract["readiness"]["releaseProfiles"][0]["id"] = "stable-v1"
    for item in contract["workItems"]:
        item["profileApplicability"] = {"stable-v1": value for value in item["profileApplicability"].values()}
    contract["workItems"][0]["requiredCiJobs"] = ["required-build"]
    contract["workItems"][0]["acceptanceStatus"] = [{
        "criterionDigest": "sha256:test",
        "state": "evidenced",
        "evidence": [f"ci:build.yml/101@{CANDIDATE}"],
    }]
    return contract


class AcceptanceShaUnitTests(unittest.TestCase):
    """Pure mutation tests that do not need the exact-CI temporary file."""

    def check(self, evidence):
        contract = {"workItems": [{"id": "item", "acceptanceStatus": [{
            "state": "evidenced", "evidence": evidence,
        }]}]}
        return rq.acceptance_sha_errors(contract, {"item"}, CANDIDATE)

    def test_empty_or_malformed_acceptance_evidence_is_refused(self):
        self.assertTrue(rq.acceptance_sha_errors(
            {"workItems": [{"id": "item", "acceptanceStatus": [{"state": "evidenced", "evidence": []}]}]},
            {"item"}, CANDIDATE,
        ))
        self.assertTrue(self.check(["ci:build.yml/latest@main"]))

    def test_stale_acceptance_evidence_is_refused(self):
        errors = self.check(["ci:build.yml/7@" + "2" * 40])
        self.assertTrue(any("not bound to candidate SHA" in error for error in errors), errors)

    def test_candidate_acceptance_evidence_is_accepted(self):
        self.assertEqual(self.check(["README.md", "ci:build.yml/7@" + CANDIDATE]), [])


class ReleaseQualificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        self.exact_ci = self.write_gate(gate_lines())

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def write_gate(self, lines: list[str], name: str = "exact-ci.out") -> Path:
        path = self.root / name
        path.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
        return path

    def qualify(self, contract: dict, exact_ci: Path | None = None, candidate_sha: str = CANDIDATE) -> dict:
        return rq.qualify(contract, "stable-v1", candidate_sha, exact_ci or self.exact_ci, REPOSITORY, BLOCKING)

    def assert_refused(self, report: dict, needle: str) -> None:
        self.assertEqual("refused", report["verdict"])
        self.assertTrue(any(needle in error for error in report["errors"]), report["errors"])

    def test_the_gate_fixture_is_a_complete_exact_record(self) -> None:
        self.assertEqual(set(exact_evidence.GATE_OUTPUT_KEYS), set(GATE_KEY_FROM_ENV.values()))
        self.assertEqual(len(EXACT_VALUES), len(exact_evidence.GATE_OUTPUT_KEYS))

    def test_qualified_candidate_writes_a_byte_stable_closed_report(self) -> None:
        payloads = []
        for name in ("first.json", "second.json"):
            report = self.qualify(qualifying_contract())
            path = self.root / name
            rq.write_new(path, rq.report_bytes(report))
            payloads.append(path.read_bytes())
        self.assertEqual(payloads[0], payloads[1])
        report = json.loads(payloads[0])
        self.assertEqual(
            {"schema", "stage", "candidateSha", "exactCiManifestDigest", "checkedItems", "errors", "verdict"},
            set(report),
        )
        self.assertEqual("spark.release-qualification/1", report["schema"])
        self.assertEqual("qualified", report["verdict"])
        self.assertEqual([], report["errors"])
        self.assertEqual(["build"], report["checkedItems"])
        self.assertRegex(report["exactCiManifestDigest"], r"^sha256:[0-9a-f]{64}$")

    def test_unfinished_transitive_dependency_is_refused(self) -> None:
        contract = qualifying_contract()
        contract["workItems"][0]["dependencies"] = ["hidden"]
        contract["workItems"].append({"id": "hidden", "status": "open", "dependencies": []})
        report = self.qualify(contract)
        self.assert_refused(report, "unfinished qualification item or transitive dependency hidden")
        self.assertIn("hidden", report["checkedItems"])

    def test_required_job_outside_the_blocking_set_is_refused(self) -> None:
        contract = qualifying_contract()
        contract["workItems"][0]["requiredCiJobs"] = ["required-build", "scheduled-soak"]
        self.assert_refused(
            self.qualify(contract), "build: required CI job scheduled-soak can be skipped without blocking publication"
        )

    def test_required_job_of_a_transitive_dependency_is_checked(self) -> None:
        contract = qualifying_contract()
        contract["workItems"][0]["dependencies"] = ["hidden"]
        contract["workItems"].append(
            {"id": "hidden", "status": "done", "dependencies": [], "requiredCiJobs": ["advisory-lane"],
             "acceptanceStatus": [{"state": "evidenced", "evidence": [f"ci:build.yml/102@{CANDIDATE}"]}]}
        )
        self.assert_refused(self.qualify(contract), "hidden: required CI job advisory-lane can be skipped")

    def test_acceptance_ci_evidence_must_name_the_candidate_sha(self) -> None:
        contract = qualifying_contract()
        contract["workItems"][0]["acceptanceStatus"] = [{
            "criterionDigest": "sha256:test",
            "state": "evidenced",
            "evidence": ["ci:build.yml/77@" + "2" * 40],
        }]
        self.assert_refused(self.qualify(contract), "is not bound to candidate SHA")

    def test_missing_exact_ci_field_is_refused(self) -> None:
        exact_ci = self.write_gate(gate_lines()[1:], "missing.out")
        self.assert_refused(self.qualify(qualifying_contract(), exact_ci), "exact-gate output is incomplete")

    def test_line_broken_or_duplicate_exact_ci_field_is_refused(self) -> None:
        lines = gate_lines()
        broken = [lines[0] + "\rinjected=1", *lines[1:]]
        duplicated = [*lines, lines[0]]
        for label, payload in (("line break", broken), ("duplicate", duplicated)):
            with self.subTest(label):
                exact_ci = self.write_gate(payload, f"{label.replace(' ', '-')}.out")
                self.assert_refused(self.qualify(qualifying_contract(), exact_ci), "unknown, duplicate, or empty")

    def test_malformed_candidate_sha_is_refused(self) -> None:
        for value in ("1" * 39, "A" * 40, "Working"):
            with self.subTest(value):
                self.assert_refused(self.qualify(qualifying_contract(), candidate_sha=value), "candidate SHA")

    def test_existing_report_is_never_replaced(self) -> None:
        output = self.root / "report.json"
        output.write_bytes(b"prior")
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            status = rq.main([
                "--stage", "stable-v1", "--candidate-sha", CANDIDATE, "--exact-ci", str(self.exact_ci),
                "--output", str(output), "--repository", REPOSITORY,
            ])
        self.assertEqual(1, status)
        self.assertEqual(b"prior", output.read_bytes())
        self.assertIn("already exists; refusing to replace it", stderr.getvalue())

    def test_repository_contract_is_refused_and_names_its_blockers(self) -> None:
        # REL-190 is blocked at HEAD, so the real contract must fail; a report
        # that passed here would prove the check had stopped checking.
        output = self.root / "real.json"
        with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
            status = rq.main([
                "--stage", "stable-v1", "--candidate-sha", CANDIDATE, "--exact-ci", str(self.exact_ci),
                "--output", str(output), "--repository", REPOSITORY,
            ])
        self.assertEqual(1, status)
        report = json.loads(output.read_bytes())
        self.assertEqual("refused", report["verdict"])
        self.assertIn("REL-190", report["checkedItems"])
        self.assertNotIn("REL-200", report["checkedItems"])
        self.assertTrue(any("REL-190" in error for error in report["errors"]), report["errors"])
        self.assertEqual(report["errors"], sorted(report["errors"]))
        self.assertIsNotNone(report["exactCiManifestDigest"])

    def test_repository_predecessor_stage_is_refused(self) -> None:
        report = rq.qualify(load_contract(), "predecessor", CANDIDATE, self.exact_ci, REPOSITORY,
                            rq.publication_blocking_jobs())
        self.assertEqual("refused", report["verdict"])
        self.assertIn("REL-191", report["checkedItems"])
        self.assertNotIn("REL-193", report["checkedItems"])

    def test_blocking_jobs_cover_the_gate_the_exact_verifier_and_the_release_chain(self) -> None:
        blocking = rq.publication_blocking_jobs()
        definitions = rq.workflow_job_definitions()
        # Every required-ci-gate need that no other workflow redefines.
        unshared = {job for job in contract_selectors.required_gate_jobs() if definitions[job] == {"build.yml"}}
        self.assertTrue(unshared <= blocking)
        # build-installer blocks in both build.yml and release.yml, so it stays.
        for job in ("profile-required-gates", "build-installer", "prepare", "verify"):
            self.assertIn(job, blocking)
        self.assertNotIn("release", blocking)
        self.assertNotIn("verify-stable-publication", blocking)

    def test_a_name_with_a_non_blocking_definition_elsewhere_is_not_blocking(self) -> None:
        # release.yml's build-macos is on the release needs chain, but build.yml's
        # build-macos is continue-on-error and not needed by required-ci-gate;
        # analyze (codeql.yml vs msvc.yml) and report (codeql-report.yml vs
        # codacy-report.yml) pair a certified job with an unrelated one.
        definitions = rq.workflow_job_definitions()
        self.assertEqual({"build.yml", "release.yml"}, set(definitions["build-macos"]))
        self.assertNotIn("build-macos", contract_selectors.required_gate_jobs())
        blocking = rq.publication_blocking_jobs()
        for job in ("build-macos", "analyze", "report"):
            with self.subTest(job=job):
                self.assertGreater(len(definitions[job]), 1)
                self.assertNotIn(job, blocking)

    def test_blocking_names_are_decided_per_workflow_definition(self) -> None:
        pairs = frozenset({("release.yml", "shared"), ("build.yml", "both"), ("release.yml", "both"),
                           ("release.yml", "only")})
        definitions = {
            "shared": frozenset({"release.yml", "build.yml"}),
            "both": frozenset({"release.yml", "build.yml"}),
            "only": frozenset({"release.yml"}),
        }
        self.assertEqual({"both", "only"}, set(rq.blocking_job_names(pairs, definitions)))

    def test_work_item_requiring_build_macos_is_refused(self) -> None:
        contract = qualifying_contract()
        contract["workItems"][0]["requiredCiJobs"] = ["build-macos"]
        report = rq.qualify(contract, "stable-v1", CANDIDATE, self.exact_ci, REPOSITORY,
                            rq.publication_blocking_jobs())
        self.assert_refused(report, "build: required CI job build-macos can be skipped without blocking publication")

    def test_needs_parser_reads_inline_scalar_and_block_forms(self) -> None:
        workflow = self.root / "workflow.yml"
        workflow.write_text(
            "name: probe\n"
            "on: push\n"
            "jobs:\n"
            "  a:\n    runs-on: x\n"
            "  b:\n    needs: a\n"
            "  c:\n    needs: [a, b]\n"
            "  d:\n    needs:\n      - c\n      # comment\n      - b\n    runs-on: x\n"
            "  release:\n    needs: d\n",
            encoding="utf-8",
        )
        needs = contract_selectors.workflow_job_needs(workflow)
        self.assertEqual(
            {"a": set(), "b": {"a"}, "c": {"a", "b"}, "d": {"b", "c"}, "release": {"d"}},
            {job: set(value) for job, value in needs.items()},
        )
        self.assertEqual({"a", "b", "c", "d"}, set(rq.release_needs_chain(needs)))


if __name__ == "__main__":
    unittest.main()
