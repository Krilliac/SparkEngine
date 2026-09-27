#!/usr/bin/env python3
"""SEC-110: release.yml reconciles every package with the dependency lock.

Each package build job (build-windows, build-linux, build-macos) must run
``tools/generate-sbom.py reconcile`` after the package is installed and before
CPack packages it, and retain the JSON report. The release job must require a
passing report for every package its channel publishes, generate and reproduce
the lock-bound SPDX SBOM from the exact source commit, and retain both as the
release's supply-chain evidence, all before the release assets are collected.

The workflow is read as text (no PyYAML) so the suite runs on every CTest lane.
The release job's embedded report verifier is extracted and executed against
passing, failing, missing and extra report sets.

Run:  python3 Tests/Tools/test_release_supply_chain_wiring.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"
RECONCILE_STEP = "Reconcile staged package inventory with the dependency lock"
INSTALLED_RECONCILE_STEP = "Reconcile installed package inventory with the dependency lock"
RELEASE_VERIFY_STEP = "Verify package reconciliation and generate lock-bound SPDX SBOM"
PACKAGE_JOBS = {
    "build-windows": ("Windows", RECONCILE_STEP),
    "build-linux": ("Linux", INSTALLED_RECONCILE_STEP),
    "build-macos": ("macOS", INSTALLED_RECONCILE_STEP),
}


def job_section(workflow: str, job: str) -> str:
    match = re.search(rf"^  {re.escape(job)}:\n", workflow, re.MULTILINE)
    if match is None:
        return ""
    following = re.search(r"^  [A-Za-z0-9_-]+:\n", workflow[match.end():], re.MULTILINE)
    return workflow[match.start(): match.end() + following.start() if following else len(workflow)]


def steps_of(job: str) -> list[tuple[str, str]]:
    """(name, body) of each top-level step of a job, in order."""
    starts = [match for match in re.finditer(r"^    - name: (.+)\n", job, re.MULTILINE)]
    steps = []
    for index, match in enumerate(starts):
        end = starts[index + 1].start() if index + 1 < len(starts) else len(job)
        steps.append((match.group(1).strip(), job[match.start():end]))
    return steps


def step_index(steps: list[tuple[str, str]], predicate) -> int:
    return next((index for index, (name, body) in enumerate(steps) if predicate(name, body)), -1)


def wiring_errors(workflow: str) -> list[str]:
    errors: list[str] = []
    for job_name, (platform, reconcile_name) in PACKAGE_JOBS.items():
        steps = steps_of(job_section(workflow, job_name))
        if not steps:
            errors.append(f"{job_name}: job not found")
            continue
        install = step_index(steps, lambda name, body: "cmake --install" in body)
        reconcile = step_index(steps, lambda name, body, n=reconcile_name: name == n)
        cpack = step_index(steps, lambda name, body: name == "Generate CPack packages")
        upload = step_index(steps, lambda name, body: name == "Upload package reconciliation report")
        if reconcile < 0:
            errors.append(f"{job_name}: no '{reconcile_name}' step")
            continue
        body = steps[reconcile][1]
        if "tools/generate-sbom.py reconcile" not in body:
            errors.append(f"{job_name}: reconcile step does not run generate-sbom.py reconcile")
        report = f"reconcile-{platform}-${{{{ matrix.config }}}}.json"
        if "--json-report" not in body or report not in body:
            errors.append(f"{job_name}: reconcile step does not write {report}")
        if "continue-on-error" in body or "|| true" in body:
            errors.append(f"{job_name}: reconcile step suppresses failure")
        if platform == "Windows":
            if "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }" not in body:
                errors.append(f"{job_name}: reconcile exit status is not propagated")
            if "--package-root \"${{ github.workspace }}/stage\"" not in body:
                errors.append(f"{job_name}: reconcile does not read the staged package")
        else:
            if "set -euo pipefail" not in body:
                errors.append(f"{job_name}: reconcile shell does not fail closed")
            if "--install-manifest build/install_manifest.txt" not in body:
                errors.append(f"{job_name}: reconcile does not read the install manifest")
        if not 0 <= install < reconcile:
            errors.append(f"{job_name}: reconcile does not run after the package install")
        if not reconcile < cpack:
            errors.append(f"{job_name}: reconcile does not run before CPack")
        if not reconcile < upload:
            errors.append(f"{job_name}: reconciliation report is not uploaded after the reconcile step")
        else:
            upload_body = steps[upload][1]
            if f"name: package-reconciliation-{platform}-${{{{ matrix.config }}}}" not in upload_body:
                errors.append(f"{job_name}: reconciliation report artifact is misnamed")
            if report not in upload_body:
                errors.append(f"{job_name}: upload does not carry {report}")

    release = steps_of(job_section(workflow, "release"))
    download = step_index(release, lambda name, body: name == "Download package reconciliation reports")
    verify = step_index(release, lambda name, body: name == RELEASE_VERIFY_STEP)
    retain = step_index(release, lambda name, body: name == "Retain release supply-chain evidence")
    collect = step_index(release, lambda name, body: name == "Collect release assets")
    if min(download, verify, retain, collect) < 0:
        errors.append("release: reconciliation download, verification, evidence upload or asset collection is missing")
        return errors
    if not download < verify < retain < collect:
        errors.append("release: supply-chain evidence is not verified and retained before assets are collected")
    download_body = release[download][1]
    if ("'package-reconciliation-Windows-MinSizeRel'" not in download_body
            or "'package-reconciliation-*'" not in download_body):
        errors.append("release: reconciliation reports are not selected by the channel boundary")
    verify_body = release[verify][1]
    for required in ('generate-sbom.py --source-sha "$GITHUB_SHA" --out supply-chain/SparkEngine-Lock-SBOM.spdx.json',
                     "generate-sbom.py --check supply-chain/SparkEngine-Lock-SBOM.spdx.json",
                     "set -euo pipefail"):
        if required not in verify_body:
            errors.append(f"release: verification step lacks {required!r}")
    retain_body = release[retain][1]
    if "path: supply-chain/" not in retain_body or "if-no-files-found: error" not in retain_body:
        errors.append("release: supply-chain evidence is not retained fail-closed")
    return errors


def embedded_verifier(workflow: str) -> str:
    release = steps_of(job_section(workflow, "release"))
    body = release[step_index(release, lambda name, body: name == RELEASE_VERIFY_STEP)][1]
    script = textwrap.dedent(body.split("run: |\n", 1)[1])
    return script.split("python3 - <<'PY'\n", 1)[1].split("\nPY\n", 1)[0]


class ReleaseSupplyChainWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8").replace("\r\n", "\n")

    def test_release_workflow_reconciles_every_package(self) -> None:
        self.assertEqual([], wiring_errors(self.workflow))

    def test_windows_declares_configuration_absent_components_per_profile(self) -> None:
        body = dict(steps_of(job_section(self.workflow, "build-windows")))[RECONCILE_STEP]
        self.assertIn(
            "SPARK_NOT_CONFIGURED: ${{ matrix.profile == 'stable-v1' && 'SDL2 AngelScript glad' || 'SDL2' }}", body
        )
        self.assertIn("@notConfigured", body)

    def test_removed_reconcile_step_is_detected(self) -> None:
        for job_name, (_, step_name) in PACKAGE_JOBS.items():
            with self.subTest(job=job_name):
                job = job_section(self.workflow, job_name)
                step = dict(steps_of(job))[step_name]
                mutated = self.workflow.replace(job, job.replace(step, ""), 1)
                self.assertTrue(any(error.startswith(f"{job_name}: no ") for error in wiring_errors(mutated)))

    def test_reconcile_after_cpack_is_detected(self) -> None:
        for job_name, (_, step_name) in PACKAGE_JOBS.items():
            with self.subTest(job=job_name):
                job = job_section(self.workflow, job_name)
                steps = dict(steps_of(job))
                reconcile, cpack = steps[step_name], steps["Generate CPack packages"]
                reordered = job.replace(reconcile, "", 1).replace(cpack, cpack + reconcile, 1)
                mutated = self.workflow.replace(job, reordered, 1)
                self.assertIn(f"{job_name}: reconcile does not run before CPack", wiring_errors(mutated))

    def test_suppressed_reconcile_failure_is_detected(self) -> None:
        job = job_section(self.workflow, "build-linux")
        step = dict(steps_of(job))[INSTALLED_RECONCILE_STEP]
        suppressed = step.replace('reconcile-Linux-${{ matrix.config }}.json"\n',
                                  'reconcile-Linux-${{ matrix.config }}.json" || true\n', 1)
        self.assertNotEqual(step, suppressed)
        mutated = self.workflow.replace(job, job.replace(step, suppressed), 1)
        self.assertIn("build-linux: reconcile step suppresses failure", wiring_errors(mutated))

    def test_release_without_lock_sbom_is_detected(self) -> None:
        mutated = self.workflow.replace(
            'python3 tools/generate-sbom.py --source-sha "$GITHUB_SHA" --out supply-chain/SparkEngine-Lock-SBOM.spdx.json\n',
            "", 1)
        self.assertNotEqual(mutated, self.workflow)
        self.assertTrue(any("SparkEngine-Lock-SBOM" in error for error in wiring_errors(mutated)))

    def test_evidence_retained_after_asset_collection_is_detected(self) -> None:
        release = job_section(self.workflow, "release")
        steps = dict(steps_of(release))
        retain = steps["Retain release supply-chain evidence"]
        collect = steps["Collect release assets"]
        reordered = release.replace(retain, "", 1).replace(collect, collect + retain, 1)
        mutated = self.workflow.replace(release, reordered, 1)
        self.assertIn("release: supply-chain evidence is not verified and retained before assets are collected",
                      wiring_errors(mutated))

    def run_verifier(self, reports: dict[str, dict], *, versioned: bool) -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw) / "supply-chain" / "reconciliation"
            root.mkdir(parents=True)
            for name, report in reports.items():
                (root / f"reconcile-{name}.json").write_text(json.dumps(report), encoding="utf-8")
            return subprocess.run(
                [sys.executable, "-c", embedded_verifier(self.workflow)], cwd=raw, text=True, capture_output=True,
                env={**os.environ, "IS_VERSIONED": "true" if versioned else "false",
                     "BUILD_CONFIGS": '["Debug","Release"]'},
            )

    @staticmethod
    def passing() -> dict:
        return {"schema": "spark-package-inventory-reconciliation-v1", "packageFiles": 12,
                "components": {"Jolt Physics": 3}, "errors": []}

    def test_embedded_verifier_accepts_complete_passing_channel(self) -> None:
        stable = self.run_verifier({"Windows-MinSizeRel": self.passing()}, versioned=True)
        self.assertEqual(stable.returncode, 0, stable.stderr)
        nightly_names = ["Windows-Debug", "Windows-Release", "Linux-Debug", "Linux-Release",
                         "macOS-Debug", "macOS-Release"]
        nightly = self.run_verifier({name: self.passing() for name in nightly_names}, versioned=False)
        self.assertEqual(nightly.returncode, 0, nightly.stderr)

    def test_embedded_verifier_rejects_failed_missing_extra_or_foreign_reports(self) -> None:
        failed = dict(self.passing(), errors=["x: unmapped third-party payload"])
        empty = dict(self.passing(), components={})
        foreign = dict(self.passing(), schema="something-else")
        cases = {
            "failed": {"Windows-MinSizeRel": failed},
            "no components": {"Windows-MinSizeRel": empty},
            "foreign schema": {"Windows-MinSizeRel": foreign},
            "missing": {},
            "extra": {"Windows-MinSizeRel": self.passing(), "Linux-Release": self.passing()},
        }
        for label, reports in cases.items():
            with self.subTest(case=label):
                result = self.run_verifier(reports, versioned=True)
                self.assertNotEqual(result.returncode, 0, label)


if __name__ == "__main__":
    unittest.main()
