#!/usr/bin/env python3
"""Contract tests for fail-closed required workflow execution."""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"
RELEASE_RECOVERY_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release-recovery.yml"
LOC_COUNTER_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "loc-counter.yml"
SITE_DATA_PUBLISH_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "site-data-publish.yml"
TRUSTED_CI_AGGREGATE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "trusted-ci-aggregate.yml"
README = REPO_ROOT / "README.md"
TEST_COUNT_RATCHET = REPO_ROOT / ".github" / "test-count-ratchet.json"
TESTS_CMAKE = REPO_ROOT / "Tests" / "CMakeLists.txt"
TELEMETRY_EXPECTED_COUNT = 7


def step_blocks(workflow: str) -> list[tuple[str, str]]:
    """Return named YAML step blocks at any job indentation depth."""

    lines = workflow.splitlines()
    starts: list[tuple[int, int, str]] = []
    for index, line in enumerate(lines):
        match = re.match(r"^(?P<indent> +)-\s+name:\s*(?P<name>.*)$", line)
        if match:
            starts.append((index, len(match.group("indent")), match.group("name")))

    blocks: list[tuple[str, str]] = []
    for start, indent, raw_name in starts:
        end = len(lines)
        for candidate in range(start + 1, len(lines)):
            line = lines[candidate]
            if not line.strip():
                continue
            candidate_indent = len(line) - len(line.lstrip(" "))
            if candidate_indent < indent:
                end = candidate
                break
            if candidate_indent == indent and re.match(r"^\s*-\s+", line):
                end = candidate
                break
        name = raw_name.strip().strip('"')
        blocks.append((name, "\n".join(lines[start:end])))
    return blocks


def status_is_enforced(block: str, variable: str) -> bool:
    """Return whether a captured producer status ultimately controls an exit."""

    reference = rf'\$(?:\{{{re.escape(variable)}\}}|{re.escape(variable)}\b)'
    if re.search(rf'(?m)^\s*exit\s+["\']?{reference}["\']?\s*$', block):
        return True

    for match in re.finditer(r"(?ms)^\s*if\s+(?P<condition>[^\n]+)\n(?P<body>.*?)^\s*fi\s*$", block):
        if re.search(reference, match.group("condition")) and re.search(
            r"(?m)^\s*exit\s+(?:[1-9][0-9]*|[^\n]*\$)", match.group("body")
        ):
            return True
    return False


def has_explicit_pipeline_status(block: str) -> bool:
    """Return whether every substantive tee pipeline has enforced status."""

    lines = block.splitlines()
    tee_indices = [
        index
        for index, line in enumerate(lines)
        if re.search(r"\|\s*tee\b", line)
        and not re.match(r"^\s*(?:echo|printf)\b", line)
    ]
    captured_variables: list[str] = []
    for tee_index in tee_indices:
        capture_match = None
        for candidate in lines[tee_index + 1 :]:
            if not candidate.strip() or candidate.lstrip().startswith("#"):
                continue
            capture_match = re.match(
                r"^\s*([A-Za-z_][A-Za-z0-9_]*)=\$\{PIPESTATUS\[0\]\}\s*$",
                candidate,
            )
            break
        if capture_match is None:
            return False
        captured_variables.append(capture_match.group(1))

    return bool(tee_indices) and all(
        status_is_enforced(block, variable) for variable in captured_variables
    )


def pipefail_precedes_every_pipeline(block: str) -> bool:
    """Track active pipefail state and require it before each tee pipeline."""

    pipefail_active = False
    saw_pipeline = False
    for line in block.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if re.match(r"^set\s+\+[a-z]*o\s+pipefail\b", stripped):
            pipefail_active = False
            continue
        if re.match(r"^set\s+-[a-z]*o\s+pipefail\b", stripped):
            pipefail_active = True
            continue
        if re.search(r"\|\s*tee\b", line) and not re.match(
            r"^\s*(?:echo|printf)\b", line
        ):
            saw_pipeline = True
            if not pipefail_active:
                return False
    return saw_pipeline


def unprotected_tee_steps(workflow: str) -> list[str]:
    """Find steps whose producer status can be hidden by ``tee``."""

    failures: list[str] = []
    for name, block in step_blocks(workflow):
        if re.search(r"\|\s*tee\b", block) is None:
            continue
        captures_pipeline_status = "PIPESTATUS[0]" in block
        if captures_pipeline_status and has_explicit_pipeline_status(block):
            continue
        if not captures_pipeline_status and pipefail_precedes_every_pipeline(block):
            continue
        failures.append(name)
    return failures


def named_step(workflow: str, name: str) -> str:
    matches = [block for step_name, block in step_blocks(workflow) if step_name == name]
    if len(matches) != 1:
        raise AssertionError(f"expected exactly one workflow step named {name!r}, found {len(matches)}")
    return matches[0]


def yaml_section(workflow: str, key: str, *, indent: int) -> str:
    """Return one mapping entry using indentation, rejecting duplicate keys."""

    lines = workflow.splitlines()
    pattern = re.compile(rf"^{' ' * indent}{re.escape(key)}:\s*(?:#.*)?$")
    starts = [index for index, line in enumerate(lines) if pattern.match(line)]
    if len(starts) != 1:
        raise AssertionError(f"expected exactly one YAML mapping {key!r} at indent {indent}")
    start = starts[0]
    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        candidate_indent = len(line) - len(line.lstrip(" "))
        if candidate_indent <= indent:
            end = index
            break
    return "\n".join(lines[start:end])


def exact_field(block: str, field: str, value: str, *, indent: int = 4) -> bool:
    pattern = re.compile(
        rf"^{' ' * indent}{re.escape(field)}:\s*(.*?)\s*(?:#.*)?$",
        re.MULTILINE,
    )
    matches = pattern.findall(block)
    return len(matches) == 1 and matches[0] == value


def telemetry_ctest_contract_errors(cmake: str) -> list[str]:
    """Validate the exact executable selector behind the required telemetry job."""

    errors: list[str] = []
    marker = "add_test(NAME TelemetrySpool"
    if cmake.count(marker) != 1:
        return ["Tests/CMakeLists.txt must register TelemetrySpool exactly once"]

    start = cmake.index(marker)
    end = cmake.find("\nadd_test(", start + len(marker))
    if end == -1:
        end = len(cmake)
    block = cmake[start:end]

    expected_command = "add_test(NAME TelemetrySpool COMMAND $<TARGET_FILE:SparkTests> --warn-is-error)"
    expected_environment = (
        'ENVIRONMENT "SPARK_TEST_NAME=Telemetry_SpoolRecovery;'
        f'SPARK_TEST_EXPECT_COUNT={TELEMETRY_EXPECTED_COUNT}"'
    )
    for fragment in (expected_command, expected_environment, 'LABELS "telemetry-integration"', "TIMEOUT 30"):
        if block.count(fragment) != 1:
            errors.append(f"TelemetrySpool CTest is missing/duplicating {fragment}")
    if "--quiet" in block:
        errors.append("TelemetrySpool CTest must not suppress selected-test evidence")
    if cmake.count("TestTelemetrySpool.cpp") != 1:
        errors.append("SparkTests must compile TestTelemetrySpool.cpp exactly once")
    return errors


def versioned_publication_gate_errors(workflow: str) -> list[str]:
    """Validate the exact fail-closed gate before versioned publication."""

    errors: list[str] = []
    step_name = "Verify stable-v1 is ready for versioned publication"
    try:
        readiness = named_step(workflow, step_name)
    except AssertionError as error:
        return [str(error)]

    if not exact_field(
        readiness,
        "if",
        "needs.prepare.outputs.is_versioned == 'true'",
        indent=6,
    ):
        errors.append("stable-v1 publication gate must use the exact versioned-release condition")
    if not exact_field(
        readiness,
        "run",
        "python3 tools/site-data/validate.py --require-ready",
        indent=6,
    ):
        errors.append("stable-v1 publication gate must run the exact readiness validator")
    if not exact_field(readiness, "shell", "bash", indent=6):
        errors.append("stable-v1 publication gate must use the exact bash shell contract")
    if re.search(
        r'''(?mx)^\s+(?:continue-on-error|'continue-on-error'|"continue-on-error")\s*:''',
        readiness,
    ):
        errors.append("stable-v1 publication gate must not continue on error")

    try:
        required_ci = named_step(
            workflow,
            "Verify exact source commit passed Required CI Gate",
        )
        badge_checkout = named_step(workflow, "Checkout badge-state seed")
    except AssertionError as error:
        errors.append(str(error))
    else:
        ordered_step_names = [name for name, _block in step_blocks(workflow)]
        required_ci_position = ordered_step_names.index(
            "Verify exact source commit passed Required CI Gate"
        )
        profile_gate_position = ordered_step_names.index(step_name)
        badge_checkout_position = ordered_step_names.index("Checkout badge-state seed")
        if profile_gate_position != required_ci_position + 1:
            errors.append(
                "stable-v1 publication gate must run immediately after Required CI"
            )
        if profile_gate_position >= badge_checkout_position:
            errors.append("stable-v1 publication gate must precede badge publication")

    return errors


def release_run_timestamp_errors(workflow: str) -> list[str]:
    """Require release metadata to use the authenticated workflow-run record."""

    errors: list[str] = []
    if "github.run_started_at" in workflow:
        errors.append("release metadata must not use the undefined github.run_started_at context")

    step_name = "Resolve workflow run start time"
    try:
        timestamp_step = named_step(workflow, step_name)
    except AssertionError as error:
        return [*errors, str(error)]

    required_fragments = (
        "id: run-start",
        "GH_TOKEN: ${{ github.token }}",
        "GITHUB_REPOSITORY: ${{ github.repository }}",
        "GITHUB_RUN_ID: ${{ github.run_id }}",
        "set -euo pipefail",
        'gh api "repos/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}"',
        "--jq '.run_started_at // empty'",
        '[[ ! "$RUN_STARTED_AT" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T',
        "exit 1",
        "printf 'started_at=%s\\n' \"$RUN_STARTED_AT\" >> \"$GITHUB_OUTPUT\"",
    )
    for fragment in required_fragments:
        if timestamp_step.count(fragment) != 1:
            errors.append(f"release run timestamp step is missing/duplicating {fragment}")
    if re.search(
        r'''(?mx)^\s+(?:continue-on-error|'continue-on-error'|"continue-on-error")\s*:''',
        timestamp_step,
    ):
        errors.append("release run timestamp lookup must not continue on error")

    output_reference = "${{ steps.run-start.outputs.started_at }}"
    if workflow.count(output_reference) != 2:
        errors.append("stable and nightly release bodies must use the exact run timestamp output")

    ordered_step_names = [name for name, _block in step_blocks(workflow)]
    try:
        timestamp_position = ordered_step_names.index(step_name)
        stable_position = ordered_step_names.index(
            "Stage new or interrupted stable versioned release as draft"
        )
        nightly_position = ordered_step_names.index("Stage nightly rolling release as draft")
    except ValueError as error:
        errors.append(f"release timestamp ordering is incomplete: {error}")
    else:
        if timestamp_position >= stable_position or timestamp_position >= nightly_position:
            errors.append("release run timestamp must be resolved before either release body is staged")

    return errors


def standard_test_evidence_errors(workflow: str) -> list[str]:
    """Reject stale or partial primary-lane test evidence."""

    errors: list[str] = []
    job_names = (
        "build-windows-vs2022",
        "build-windows-vs2026",
        "build-linux-gcc",
        "build-linux-clang",
        "build-macos",
    )
    expected_conditions = {
        "build-windows-vs2022": "always() && matrix.config == 'Release'",
        "build-windows-vs2026": "always() && matrix.config == 'Release'",
        "build-linux-gcc": "always()",
        "build-linux-clang": "always()",
        "build-macos": "always() && matrix.config == 'Release'",
    }
    scrub_fragments = (
        "rm -f",
        "build/SparkTests-junit.xml",
        "build/SparkTests.log",
        "build/SparkTests-output.log",
        "build/ctest-junit.xml",
        "build/*-process-smoke.log",
        "build/*-process-smoke.json",
        "build/*-process-smoke.xml",
    )
    summary_checks = (
        "test -s build/SparkTests-junit.xml",
        "test -s build/SparkTests.log",
        "test -s build/SparkTests-output.log",
        "test -s build/ctest-junit.xml",
    )
    upload_paths = (
        "build/SparkTests-junit.xml",
        "build/SparkTests.log",
        "build/SparkTests-output.log",
        "build/ctest-junit.xml",
    )

    for job_name in job_names:
        try:
            job = yaml_section(workflow, job_name, indent=2)
        except AssertionError as error:
            errors.append(str(error))
            continue

        ordered_names = [name for name, _block in step_blocks(job)]
        required_names = (
            "Restore build directory",
            "Scrub restored test evidence",
            "Validate and summarize test statistics",
            "Upload machine-readable test results",
        )
        try:
            positions = {name: ordered_names.index(name) for name in required_names}
        except ValueError as error:
            errors.append(f"{job_name} test-evidence steps are incomplete: {error}")
            continue
        if positions["Scrub restored test evidence"] != positions["Restore build directory"] + 1:
            errors.append(f"{job_name} must scrub test evidence immediately after cache restore")
        if not (
            positions["Scrub restored test evidence"]
            < positions["Validate and summarize test statistics"]
            < positions["Upload machine-readable test results"]
        ):
            errors.append(f"{job_name} test-evidence steps are out of order")

        try:
            scrub = named_step(job, "Scrub restored test evidence")
            summary = named_step(job, "Validate and summarize test statistics")
            upload = named_step(job, "Upload machine-readable test results")
        except AssertionError as error:
            errors.append(str(error))
            continue

        if not exact_field(scrub, "if", "always()", indent=6):
            errors.append(f"{job_name} test-evidence scrub must run under exact if: always()")
        if re.search(r"(?m)^\s+['\"]?continue-on-error['\"]?:", scrub):
            errors.append(f"{job_name} test-evidence scrub suppresses failure")
        for fragment in scrub_fragments:
            if scrub.count(fragment) != 1:
                errors.append(f"{job_name} test-evidence scrub is missing/duplicating {fragment}")

        summarizer_position = summary.find("summarize-test-results.py")
        if summarizer_position < 0:
            errors.append(f"{job_name} test statistics summarizer is missing")
        for command in summary_checks:
            if summary.count(command) != 1:
                errors.append(f"{job_name} summary is missing/duplicating {command}")
            elif summary.find(command) > summarizer_position:
                errors.append(f"{job_name} validates {command} after statistics are generated")

        expected_condition = expected_conditions[job_name]
        if not exact_field(summary, "if", expected_condition, indent=6):
            errors.append(
                f"{job_name} summary must run under exact if: {expected_condition}"
            )
        if not exact_field(upload, "if", expected_condition, indent=6):
            errors.append(
                f"{job_name} test-evidence upload must run under exact if: {expected_condition}"
            )

        for path in upload_paths:
            if upload.count(path) != 1:
                errors.append(f"{job_name} upload is missing/duplicating {path}")
        if upload.count("if-no-files-found: error") != 1:
            errors.append(f"{job_name} test-evidence upload must fail when no files exist")

    return errors


def required_workflow_errors(workflow: str) -> list[str]:
    """Conservatively parse the fail-closed sanitizer/aggregation YAML contract."""

    errors: list[str] = []
    if re.search(r"(?m)^\s*<<:\s*|:\s*[&*][A-Za-z_][A-Za-z0-9_-]*\s*$", workflow):
        errors.append("YAML anchors, aliases, and merge keys are forbidden in required workflow semantics")
    try:
        triggers = yaml_section(workflow, "on", indent=0)
    except AssertionError as exc:
        errors.append(str(exc))
        triggers = ""
    if re.search(r"(?m)^\s+['\"]?paths(?:-ignore)?['\"]?:\s*", triggers):
        errors.append("workflow trigger path filters may bypass required evidence")
    try:
        concurrency = yaml_section(workflow, "concurrency", indent=0)
    except AssertionError as exc:
        errors.append(str(exc))
        concurrency = ""
    if concurrency:
        expected_cancel = "${{ github.event_name == 'pull_request' }}"
        if not exact_field(concurrency, "cancel-in-progress", expected_cancel, indent=2):
            errors.append("push evidence must not be cancelled by workflow concurrency")
        if "|| github.sha }}" not in concurrency:
            errors.append("workflow concurrency group is not bound to the pushed SHA")

    errors.extend(standard_test_evidence_errors(workflow))

    try:
        validation = yaml_section(workflow, "validate-ci-tools", indent=2)
        wrapper_harness = named_step(
            validation, "Test granular SparkTests wrapper failure recovery"
        )
    except AssertionError as error:
        errors.append(str(error))
    else:
        if re.search(
            r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", wrapper_harness
        ):
            errors.append("SparkTests wrapper recovery harness is conditional or suppresses failure")
        if not exact_field(
            wrapper_harness,
            "run",
            "python3 .github/scripts/test-run-spark-tests.py",
            indent=6,
        ):
            errors.append("SparkTests wrapper recovery harness command is not exact")

    for sanitizer, run_name, verify_name in (
        ("asan", "Run Tests under ASan + UBSan + LSan", "Verify published ASan exact-commit evidence"),
        ("tsan", "Run Tests under TSan", "Verify published TSan exact-commit evidence"),
    ):
        job_name = f"build-linux-{sanitizer}"
        try:
            job = yaml_section(workflow, job_name, indent=2)
        except AssertionError as exc:
            errors.append(str(exc))
            continue
        if not exact_field(job, "timeout-minutes", "90"):
            errors.append(f"{job_name} must have exactly timeout-minutes: 90")
        if re.search(r"(?m)^    ['\"]?(?:if|continue-on-error|strategy)['\"]?:", job):
            errors.append(f"{job_name} has a bypassing job-level directive")
        if re.search(r"(?m)^\s+['\"]?matrix['\"]?:\s*", job):
            errors.append(f"{job_name} must not be expanded through a matrix")

        try:
            runner = named_step(job, run_name)
        except AssertionError as exc:
            errors.append(str(exc))
            runner = ""
        if runner:
            if re.search(r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", runner):
                errors.append(f"{run_name} has a conditional/error bypass")
            if re.search(r"\|\|\s*true\b", runner):
                errors.append(f"{run_name} suppresses a runner failure")
            if (
                len(re.findall(r"(?<![A-Za-z0-9_-])--timeout-seconds\s+[^\s\\]+", runner)) != 1
                or runner.count("--timeout-seconds 900") != 1
            ):
                errors.append(f"{run_name} must use one exact process timeout")
            if len(re.findall(r"(?<![A-Za-z0-9_-])--warn-is-error(?![=A-Za-z0-9_-])", runner)) != 1:
                errors.append(f"{run_name} must use --warn-is-error exactly once")
            if (
                len(re.findall(r"(?<![A-Za-z0-9_-])--shuffle(?:\s+[^\s\\]+|=[^\s\\]+)", runner)) != 1
                or len(re.findall(r"(?<![A-Za-z0-9_-])--shuffle\s+123(?![0-9])", runner)) != 1
            ):
                errors.append(f"{run_name} must use --shuffle 123 exactly once")
            if re.search(r"--(?:retry|retries)(?:\b|=)", runner):
                errors.append(f"{run_name} must not enable retries")
            for fragment in (
                f"--sanitizer {sanitizer}",
                '--expected-sha "${{ github.sha }}"',
                '--run-id "${{ github.run_id }}"',
                '--run-attempt "${{ github.run_attempt }}"',
                '--job "${{ github.job }}"',
                "--expected-selector all",
                "--minimum-tests 6600",
                "--timeout-seconds 900",
            ):
                if runner.count(fragment) != 1:
                    errors.append(f"{run_name} is missing/duplicating {fragment}")

        try:
            published = named_step(job, verify_name)
        except AssertionError as exc:
            errors.append(str(exc))
            published = ""
        if published:
            if re.search(r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", published) or re.search(r"\|\|\s*true\b", published):
                errors.append(f"{verify_name} is conditional or suppresses failure")
            expected_fragments = (
                "verify-sanitizer-evidence.py verify-published",
                f"--sanitizer {sanitizer}",
                f"--lane linux-{sanitizer}",
                f"--job build-linux-{sanitizer}",
                '--expected-sha "${{ github.sha }}"',
                '--run-id "${{ github.run_id }}"',
                '--run-attempt "${{ github.run_attempt }}"',
                "--timeout-seconds 900",
                "--minimum-tests 6600",
            )
            for fragment in expected_fragments:
                if published.count(fragment) != 1:
                    errors.append(f"{verify_name} is missing/duplicating {fragment}")
            if len(re.findall(r"(?<![A-Za-z0-9_-])--timeout-seconds\s+[^\s\\]+", published)) != 1:
                errors.append(f"{verify_name} duplicates or ambiguously overrides its timeout")

    try:
        telemetry = yaml_section(workflow, "telemetry-integration", indent=2)
    except AssertionError as exc:
        errors.append(str(exc))
        telemetry = ""
    if telemetry:
        if not exact_field(telemetry, "runs-on", "ubuntu-24.04"):
            errors.append("telemetry-integration must run on ubuntu-24.04")
        if not exact_field(telemetry, "timeout-minutes", "30"):
            errors.append("telemetry-integration must have exactly timeout-minutes: 30")
        if re.search(r"(?m)^    ['\"]?(?:if|continue-on-error|strategy)['\"]?:", telemetry):
            errors.append("telemetry-integration has a bypassing job-level directive")

        required_steps = (
            (
                "Configure Linux Shipping telemetry tests",
                ("set -o pipefail", "cmake --preset linux-shipping -DBUILD_TESTS=ON"),
            ),
            (
                "Build telemetry integration target",
                ("set -o pipefail", "cmake --build --preset linux-shipping --target SparkTests"),
            ),
            (
                "Run telemetry spool integration test",
                (
                    "set -o pipefail",
                    "ctest --test-dir build/linux-shipping",
                    "--output-on-failure",
                    "--no-tests=error",
                    "-R '^TelemetrySpool$'",
                ),
            ),
        )
        for step_name, fragments in required_steps:
            try:
                step = named_step(telemetry, step_name)
            except AssertionError as exc:
                errors.append(str(exc))
                continue
            if re.search(r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", step):
                errors.append(f"{step_name} has a conditional/error bypass")
            if re.search(r"\|\|\s*true\b", step):
                errors.append(f"{step_name} suppresses failure")
            for fragment in fragments:
                if step.count(fragment) != 1:
                    errors.append(f"{step_name} is missing/duplicating {fragment}")

        try:
            error_upload = named_step(telemetry, "Upload telemetry integration error summary")
        except AssertionError as exc:
            errors.append(str(exc))
        else:
            for fragment in ("if: failure()", "name: ci-errors-telemetry-integration"):
                if error_upload.count(fragment) != 1:
                    errors.append(f"telemetry integration error upload is missing/duplicating {fragment}")

    try:
        aggregate = yaml_section(workflow, "aggregate-test-stats", indent=2)
    except AssertionError as exc:
        errors.append(str(exc))
        aggregate = ""
    if aggregate:
        if not exact_field(aggregate, "if", "always()"):
            errors.append("aggregate-test-stats must run under exact if: always()")
        download = ""
        try:
            download = named_step(aggregate, "Download primary-lane test evidence")
        except AssertionError as exc:
            errors.append(str(exc))
        if "continue-on-error" in download:
            errors.append("primary evidence download must fail closed")
        for sanitizer in ("asan", "tsan"):
            step_name = f"Verify downloaded {sanitizer.upper() if sanitizer == 'asan' else 'TSan'} evidence identity"
            # ASAN upper-casing is not the authored display name.
            if sanitizer == "asan":
                step_name = "Verify downloaded ASan evidence identity"
            try:
                block = named_step(aggregate, step_name)
            except AssertionError as exc:
                errors.append(str(exc))
                continue
            if re.search(r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", block) or re.search(r"\|\|\s*true\b", block):
                errors.append(f"{step_name} is conditional or suppresses failure")
            artifact_root = "${{ github.workspace }}/ci-test-results"
            for fragment in (
                "verify-sanitizer-evidence.py verify-published",
                f'--evidence-dir "{artifact_root}/test-results-linux-{sanitizer}"',
                f'--stats "{artifact_root}/test-results-linux-{sanitizer}/test-stats-linux-{sanitizer}.json"',
                f"--sanitizer {sanitizer}",
                f"--lane linux-{sanitizer}",
                f"--job build-linux-{sanitizer}",
                '--expected-sha "${{ github.sha }}"',
                '--run-id "${{ github.run_id }}"',
                '--run-attempt "${{ github.run_attempt }}"',
                "--allow-prior-attempt",
            ):
                if block.count(fragment) != 1:
                    errors.append(f"{step_name} is missing/duplicating {fragment}")
            if len(re.findall(r"(?<![A-Za-z0-9_-])--timeout-seconds\s+[^\s\\]+", block)) != 1:
                errors.append(f"{step_name} duplicates or ambiguously overrides its timeout")
        for dependency in ("build-linux-asan", "build-linux-tsan"):
            if len(re.findall(rf"(?m)^      - {dependency}$", aggregate)) != 1:
                errors.append(f"aggregate-test-stats must need {dependency} exactly once")

    try:
        report = yaml_section(workflow, "report-ci-errors", indent=2)
    except AssertionError as exc:
        errors.append(str(exc))
        report = ""
    if report:
        if len(re.findall(r"(?m)^      - telemetry-integration$", report)) != 1:
            errors.append("report-ci-errors must need telemetry-integration exactly once")

    try:
        gate = yaml_section(workflow, "required-ci-gate", indent=2)
    except AssertionError as exc:
        errors.append(str(exc))
        gate = ""
    if gate:
        if not exact_field(gate, "if", "always()"):
            errors.append("required-ci-gate must run under exact if: always()")
        expected_dependencies = [
            "validate-ci-tools", "check-format", "validate-prompts", "check-thirdparty-manifest",
            "build-linux-asan", "build-linux-tsan", "telemetry-integration",
            "build-windows-vs2022", "build-windows-shipping", "build-linux-gcc",
            "build-linux-clang", "coverage", "clang-tidy", "todo-count",
            "build-installer", "aggregate-test-stats",
        ]
        needs_match = re.search(
            r"(?ms)^    needs:\n(?P<body>(?:      - [a-z0-9-]+\n)+)", gate
        )
        actual_dependencies = (
            re.findall(r"(?m)^      - ([a-z0-9-]+)$", needs_match.group("body"))
            if needs_match else []
        )
        if actual_dependencies != expected_dependencies:
            errors.append("required-ci-gate must preserve the exact ordered 16-job dependency inventory")
        try:
            verifier = named_step(gate, "Verify every required job succeeded")
        except AssertionError as exc:
            errors.append(str(exc))
        else:
            if re.search(r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", verifier):
                errors.append("required-ci-gate verifier has a conditional/error bypass")
            required_environment = (
                "NEEDS_JSON: ${{ toJSON(needs) }}",
                "EXPECTED_REQUIRED_JOBS_JSON: '[\"validate-ci-tools\",\"check-format\",\"validate-prompts\",\"check-thirdparty-manifest\",\"build-linux-asan\",\"build-linux-tsan\",\"telemetry-integration\",\"build-windows-vs2022\",\"build-windows-shipping\",\"build-linux-gcc\",\"build-linux-clang\",\"coverage\",\"clang-tidy\",\"todo-count\",\"build-installer\",\"aggregate-test-stats\"]'",
                "DEFERRED_REQUIRED_FAILURES_JSON: '{\"build-windows-shipping\":\"failure\"}'",
            )
            if verifier.count("env:") != 1 or any(
                verifier.count(fragment) != 1 for fragment in required_environment
            ):
                errors.append("required-ci-gate verifier must consume the exact needs and deferred-failure policy once")
            if not exact_field(
                verifier,
                "run",
                "python3 .github/scripts/verify-required-jobs.py",
                indent=8,
            ):
                errors.append("required-ci-gate verifier must run the exact required-job script")
    return errors


class WorkflowFailurePropagationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = BUILD_WORKFLOW.read_text(encoding="utf-8")
        cls.release = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        cls.loc_counter = LOC_COUNTER_WORKFLOW.read_text(encoding="utf-8")
        cls.site_data_publish = SITE_DATA_PUBLISH_WORKFLOW.read_text(encoding="utf-8")
        cls.readme = README.read_text(encoding="utf-8")
        cls.tests_cmake = TESTS_CMAKE.read_text(encoding="utf-8")

    def test_required_workflow_semantics_are_fail_closed(self) -> None:
        self.assertEqual(required_workflow_errors(self.build), [])

    def test_static_ci120_baseline_does_not_duplicate_producer_enforcement(self) -> None:
        static_validation = yaml_section(self.build, "validate-ci-tools", indent=2)
        structural_producer = yaml_section(self.build, "build-windows-shipping", indent=2)
        enforcement_name = "- name: Enforce reviewed CI-120 findings"

        self.assertNotIn(enforcement_name, static_validation)
        self.assertEqual(structural_producer.count(enforcement_name), 1)
        self.assertIn("Compare reviewed build-matrix findings (CI-120)", static_validation)
        self.assertIn("Upload deterministic CI-120 evidence", static_validation)

    def test_release_artifact_builders_use_explicit_runner_images(self) -> None:
        for workflow in (self.build, self.release):
            self.assertNotIn("windows-latest", workflow)
            self.assertNotIn("macos-latest", workflow)
        self.assertIn("runs-on: windows-2025-vs2026", self.build)
        self.assertIn("- os: windows-2022", self.build)
        self.assertIn("- os: macos-15", self.build)
        self.assertIn("- os: windows-2022", self.release)
        self.assertIn("- os: macos-15", self.release)

    def test_standard_test_evidence_rejects_scrub_removal_or_reordering(self) -> None:
        vs2022 = yaml_section(self.build, "build-windows-vs2022", indent=2)
        scrub = named_step(vs2022, "Scrub restored test evidence")
        configure = named_step(vs2022, "Configure CMake (VS 2022 / v143)")
        removed = self.build.replace(scrub, "", 1)
        moved = self.build.replace(scrub, "", 1).replace(
            configure,
            f"{configure}\n{scrub}",
            1,
        )
        for label, mutated in (("removed", removed), ("moved", moved)):
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.build, "mutation fixture did not alter YAML")
                self.assertTrue(standard_test_evidence_errors(mutated), label)

    def test_standard_test_evidence_rejects_missing_per_file_postcondition(self) -> None:
        mutated = self.build.replace("        test -s build/SparkTests.log\n", "", 1)
        self.assertNotEqual(mutated, self.build, "mutation fixture did not alter YAML")
        self.assertTrue(standard_test_evidence_errors(mutated))

    def test_standard_test_evidence_scrub_must_run_after_failed_restore(self) -> None:
        mutated = self.build.replace(
            "    - name: Scrub restored test evidence\n      if: always()\n",
            "    - name: Scrub restored test evidence\n",
            1,
        )
        self.assertNotEqual(mutated, self.build, "mutation fixture did not alter YAML")
        self.assertTrue(standard_test_evidence_errors(mutated))

    def test_standard_test_evidence_summary_and_upload_must_run_after_failure(self) -> None:
        vs2022 = yaml_section(self.build, "build-windows-vs2022", indent=2)
        summary = named_step(vs2022, "Validate and summarize test statistics")
        upload = named_step(vs2022, "Upload machine-readable test results")
        mutations = {
            "summary": self.build.replace(
                summary,
                summary.replace("if: always()", "if: success()", 1),
                1,
            ),
            "upload": self.build.replace(
                upload,
                upload.replace("if: always()", "if: success()", 1),
                1,
            ),
        }
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.build, "mutation fixture did not alter YAML")
                self.assertTrue(standard_test_evidence_errors(mutated), label)

    def test_required_workflow_rejects_removed_wrapper_recovery_harness(self) -> None:
        validation = yaml_section(self.build, "validate-ci-tools", indent=2)
        harness = named_step(validation, "Test granular SparkTests wrapper failure recovery")
        mutated = self.build.replace(harness, "", 1)
        self.assertNotEqual(mutated, self.build, "mutation fixture did not alter YAML")
        self.assertTrue(required_workflow_errors(mutated))

    def test_telemetry_ctest_selector_is_fail_closed(self) -> None:
        self.assertEqual(telemetry_ctest_contract_errors(self.tests_cmake), [])

    def test_telemetry_ctest_selector_rejects_hostile_mutations(self) -> None:
        mutations = {
            "missing source": self.tests_cmake.replace("    TestTelemetrySpool.cpp\n", "", 1),
            "warning downgrade": self.tests_cmake.replace(" --warn-is-error)", ")", 1),
            "selector widened": self.tests_cmake.replace(
                "SPARK_TEST_NAME=Telemetry_SpoolRecovery",
                "SPARK_TEST_NAME=Telemetry_",
                1,
            ),
            "selected count reduced": self.tests_cmake.replace(
                f"SPARK_TEST_EXPECT_COUNT={TELEMETRY_EXPECTED_COUNT}",
                f"SPARK_TEST_EXPECT_COUNT={TELEMETRY_EXPECTED_COUNT - 1}",
                1,
            ),
        }
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.tests_cmake, "mutation fixture did not alter CMake")
                self.assertTrue(telemetry_ctest_contract_errors(mutated), label)

    def test_required_workflow_semantics_reject_hostile_mutations(self) -> None:
        mutations: dict[str, str] = {}
        mutations["suppressed runner"] = self.build.replace(
            "          -- build/bin/SparkTests --warn-is-error --shuffle 123",
            "          -- build/bin/SparkTests --warn-is-error --shuffle 123 || true",
            1,
        )
        mutations["continue on error"] = self.build.replace(
            "    - name: Run Tests under ASan + UBSan + LSan",
            "    - name: Run Tests under ASan + UBSan + LSan\n      continue-on-error: true",
            1,
        )
        mutations["duplicate seed"] = self.build.replace(
            "-- build/bin/SparkTests --warn-is-error --shuffle 123",
            "-- build/bin/SparkTests --warn-is-error --shuffle 123 --shuffle 999",
            1,
        )
        mutations["weakened timeout"] = self.build.replace(
            "build-linux-asan:\n    runs-on: ubuntu-24.04\n    timeout-minutes: 90",
            "build-linux-asan:\n    runs-on: ubuntu-24.04\n    timeout-minutes: 91",
            1,
        )
        mutations["conditional runner"] = self.build.replace(
            "    - name: Run Tests under ASan + UBSan + LSan",
            "    - name: Run Tests under ASan + UBSan + LSan\n      if: false",
            1,
        )
        mutations["matrix bypass"] = self.build.replace(
            "build-linux-asan:\n    runs-on: ubuntu-24.04",
            "build-linux-asan:\n    strategy:\n      matrix:\n        enabled: [false]\n    runs-on: ubuntu-24.04",
            1,
        )
        mutations["missing telemetry job"] = self.build.replace(
            "  telemetry-integration:",
            "  telemetry-integration-disabled:",
            1,
        )
        mutations["optional telemetry job"] = self.build.replace(
            "  telemetry-integration:\n    name: \"Telemetry Integration\"",
            "  telemetry-integration:\n    name: \"Telemetry Integration\"\n    continue-on-error: true",
            1,
        )
        mutations["telemetry zero-test bypass"] = self.build.replace(
            "ctest --test-dir build/linux-shipping --output-on-failure --no-tests=error \\\n"
            "          -R '^TelemetrySpool$'",
            "ctest --test-dir build/linux-shipping --output-on-failure \\\n"
            "          -R '^TelemetrySpool$'",
            1,
        )
        mutations["telemetry selector drift"] = self.build.replace(
            "-R '^TelemetrySpool$'",
            "-R 'Telemetry'",
            1,
        )
        mutations["telemetry report dependency removed"] = self.build.replace(
            "      - build-linux-tsan\n      - telemetry-integration\n      - build-linux-msan",
            "      - build-linux-tsan\n      - build-linux-msan",
            1,
        )
        mutations["telemetry gate dependency removed"] = self.build.replace(
            "      - build-linux-tsan\n      - telemetry-integration\n      - build-windows-vs2022",
            "      - build-linux-tsan\n      - build-windows-vs2022",
            1,
        )
        mutations["required gate verifier removed"] = self.build.replace(
            "      - name: Verify every required job succeeded",
            "      - name: Required job summary only",
            1,
        )
        mutations["required gate verifier bypassed"] = self.build.replace(
            "        run: python3 .github/scripts/verify-required-jobs.py",
            "        run: 'true'",
            1,
        )
        mutations["required gate needs evidence removed"] = self.build.replace(
            "          NEEDS_JSON: ${{ toJSON(needs) }}\n"
            "          EXPECTED_REQUIRED_JOBS_JSON:",
            "          NEEDS_JSON: '{}'\n"
            "          EXPECTED_REQUIRED_JOBS_JSON:",
            1,
        )
        mutations["path filter"] = self.build.replace(
            "  push:\n    branches: [ main, develop, Working, 'feature/**', 'claude/**', 'release/**' ]",
            "  push:\n    branches: [ main, develop, Working, 'feature/**', 'claude/**', 'release/**' ]\n    paths-ignore: ['**']",
            1,
        )
        mutations["conditional aggregate"] = self.build.replace(
            "  aggregate-test-stats:\n    name: \"Aggregate test and source statistics\"\n    if: always()",
            "  aggregate-test-stats:\n    name: \"Aggregate test and source statistics\"\n    if: false",
            1,
        )
        mutations["cancel pushed SHA"] = self.build.replace(
            "  cancel-in-progress: ${{ github.event_name == 'pull_request' }}",
            "  cancel-in-progress: true",
            1,
        )
        mutations["suppressed published verifier"] = self.build.replace(
            "          --timeout-seconds 900\n\n    - name: Extract error summary",
            "          --timeout-seconds 900 || true\n\n    - name: Extract error summary",
            1,
        )
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.build, "mutation fixture did not alter YAML")
                self.assertTrue(required_workflow_errors(mutated), label)

    def test_detector_rejects_failed_producer_hidden_by_tee(self) -> None:
        fixture = """jobs:
  test:
    steps:
    - name: Unsafe pipeline
      shell: bash
      run: |
        false | tee result.log
"""
        self.assertEqual(unprotected_tee_steps(fixture), ["Unsafe pipeline"])

    def test_detector_accepts_pipefail_or_explicit_pipeline_status(self) -> None:
        pipefail_fixture = """jobs:
  test:
    steps:
    - name: Pipefail pipeline
      run: |
        set -o pipefail
        false | tee result.log
"""
        status_fixture = """jobs:
  test:
    steps:
    - name: Explicit status pipeline
      run: |
        set +e
        false | tee result.log
        command_status=${PIPESTATUS[0]}
        exit "$command_status"
"""
        self.assertEqual(unprotected_tee_steps(pipefail_fixture), [])
        self.assertEqual(unprotected_tee_steps(status_fixture), [])

    def test_detector_rejects_captured_status_that_never_controls_exit(self) -> None:
        fixture = """jobs:
  test:
    steps:
      - name: Captured but ignored
        run: |
          set +e
          false | tee result.log
          command_status=${PIPESTATUS[0]}
          echo "$command_status"
"""
        self.assertEqual(unprotected_tee_steps(fixture), ["Captured but ignored"])

    def test_detector_rejects_commented_or_late_pipefail(self) -> None:
        commented_fixture = """jobs:
  test:
    steps:
    - name: Comment is not protection
      run: |
        # set -o pipefail
        false | tee result.log
"""
        late_fixture = """jobs:
  test:
    steps:
    - name: Late protection
      run: |
        false | tee result.log
        set -o pipefail
"""
        self.assertEqual(
            unprotected_tee_steps(commented_fixture),
            ["Comment is not protection"],
        )
        self.assertEqual(unprotected_tee_steps(late_fixture), ["Late protection"])

    def test_repository_tee_pipelines_preserve_producer_failure(self) -> None:
        for path, workflow in ((BUILD_WORKFLOW, self.build), (RELEASE_WORKFLOW, self.release)):
            with self.subTest(workflow=path.name):
                self.assertEqual(unprotected_tee_steps(workflow), [])

    def test_versioned_publication_requires_ready_release_profile(self) -> None:
        self.assertEqual(versioned_publication_gate_errors(self.release), [])

    def test_release_timestamp_uses_authenticated_workflow_run_record(self) -> None:
        self.assertEqual(release_run_timestamp_errors(self.release), [])

    def test_release_timestamp_contract_rejects_hostile_mutations(self) -> None:
        timestamp = named_step(self.release, "Resolve workflow run start time")
        mutations = {
            "undefined context": self.release.replace(
                "${{ steps.run-start.outputs.started_at }}",
                "${{ github.run_started_at }}",
                1,
            ),
            "missing token": self.release.replace(
                timestamp,
                timestamp.replace(
                    "        GH_TOKEN: ${{ github.token }}",
                    "        GH_TOKEN:",
                    1,
                ),
                1,
            ),
            "wrong endpoint": self.release.replace(
                'repos/${GITHUB_REPOSITORY}/actions/runs/${GITHUB_RUN_ID}',
                'repos/${GITHUB_REPOSITORY}/commits/${GITHUB_RUN_ID}',
                1,
            ),
            "suppressed failure": self.release.replace(
                timestamp,
                timestamp.replace("          exit 1\n", "", 1),
                1,
            ),
            "continue on error": self.release.replace(
                "      id: run-start\n",
                "      id: run-start\n      continue-on-error: true\n",
                1,
            ),
        }
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.release, "mutation fixture did not alter YAML")
                self.assertTrue(release_run_timestamp_errors(mutated), label)

    def test_versioned_publication_gate_rejects_hostile_mutations(self) -> None:
        readiness = named_step(
            self.release,
            "Verify stable-v1 is ready for versioned publication",
        )
        tag_binding = named_step(
            self.release, "Bind stable release tag to workflow commit"
        )
        gate_after_tag_binding = self.release.replace(readiness, "", 1).replace(
            tag_binding,
            f"{tag_binding}\n{readiness}",
            1,
        )
        mutations = {
            "condition bypass": self.release.replace(
                "      if: needs.prepare.outputs.is_versioned == 'true'",
                "      if: needs.prepare.outputs.is_versioned == 'true' || github.event_name == 'workflow_dispatch'",
                1,
            ),
            "suppressed validator": self.release.replace(
                "      run: python3 tools/site-data/validate.py --require-ready",
                "      run: python3 tools/site-data/validate.py --require-ready || echo ignored",
                1,
            ),
            "shell suppresses validator": self.release.replace(
                readiness,
                readiness.replace(
                    "      shell: bash",
                    "      shell: bash {0} || true",
                    1,
                ),
                1,
            ),
            "continue on error": self.release.replace(
                "      run: python3 tools/site-data/validate.py --require-ready",
                "      continue-on-error: true\n      run: python3 tools/site-data/validate.py --require-ready",
                1,
            ),
            "quoted continue on error": self.release.replace(
                "      run: python3 tools/site-data/validate.py --require-ready",
                "      'continue-on-error': true\n      run: python3 tools/site-data/validate.py --require-ready",
                1,
            ),
            "gate after tag mutation": gate_after_tag_binding,
        }
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.release, "mutation fixture did not alter YAML")
                self.assertTrue(versioned_publication_gate_errors(mutated), label)

    def test_installer_pipeline_mutations_are_detected_at_nested_indent(self) -> None:
        for path, workflow in ((BUILD_WORKFLOW, self.build), (RELEASE_WORKFLOW, self.release)):
            with self.subTest(workflow=path.name):
                installer = named_step(workflow, "Launch staged executable")
                unsafe = installer.replace("          set -o pipefail\n", "", 1)
                self.assertEqual(unprotected_tee_steps(unsafe), ["Launch staged executable"])

    def test_generated_documentation_requires_the_captured_status_to_exit(self) -> None:
        generated_docs = named_step(
            self.build,
            "Verify all generated documentation and statistics",
        )
        unsafe, replacements = re.subn(
            r'(?m)^\s*exit\s+"\$status"\s*$',
            "",
            generated_docs,
            count=1,
        )
        self.assertEqual(replacements, 1)
        self.assertEqual(
            unprotected_tee_steps(unsafe),
            ["Verify all generated documentation and statistics"],
        )

    def test_sanitizer_lane_makes_undefined_behavior_fatal(self) -> None:
        self.assertGreaterEqual(self.build.count("-fno-sanitize-recover=undefined"), 2)
        self.assertIn("UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1", self.build)
        self.assertIn("ASAN_OPTIONS: detect_leaks=1:halt_on_error=1", self.build)

    def test_sanitizer_runner_owns_private_runtime_log_prefix(self) -> None:
        self.assertEqual(self.build.count('--evidence-root "${{ runner.temp }}"'), 3)
        self.assertIn("--runtime-env ASAN_OPTIONS", self.build)
        self.assertIn("--runtime-env TSAN_OPTIONS", self.build)
        self.assertNotIn("--runtime-log-prefix", self.build)
        self.assertNotRegex(self.build, r"(?:ASAN|TSAN|MSAN)_OPTIONS:.*log_path=")

    def test_msan_is_verified_but_remains_optional(self) -> None:
        msan_block = named_step(self.build, "Run Tests under MSan")
        self.assertIn("--runtime-env MSAN_OPTIONS", msan_block)
        self.assertIn("--sanitizer msan", msan_block)

    def test_asan_tsan_are_required_msan_is_optional(self) -> None:
        gate_section = self.build[self.build.index("required-ci-gate:"):]
        gate_needs = gate_section[:gate_section.index("runs-on:")]
        self.assertIn("build-linux-asan", gate_needs)
        self.assertIn("build-linux-tsan", gate_needs)
        self.assertNotIn("build-linux-msan", gate_needs)

    def test_msan_has_continue_on_error(self) -> None:
        msan_start = self.build.index("build-linux-msan:")
        next_job = self.build.index("\n  build-", msan_start + 1)
        msan_section = self.build[msan_start:next_job]
        self.assertIn("continue-on-error: true", msan_section)

    def test_asan_does_not_have_continue_on_error(self) -> None:
        asan_start = self.build.index("build-linux-asan:")
        next_job = self.build.index("\n  build-", asan_start + 1)
        asan_section = self.build[asan_start:next_job]
        self.assertNotIn("continue-on-error", asan_section)

    def test_sanitizer_jobs_and_test_processes_have_timeouts(self) -> None:
        for sanitizer in ("asan", "tsan", "msan"):
            start = self.build.index(f"build-linux-{sanitizer}:")
            next_job = self.build.index("\n  build-", start + 1)
            section = self.build[start:next_job]
            self.assertIn("timeout-minutes: 90", section)
            self.assertIn("--timeout-seconds 900", section)

    def test_required_sanitizers_use_warning_errors_and_exact_provenance(self) -> None:
        for name in ("Run Tests under ASan + UBSan + LSan", "Run Tests under TSan"):
            block = named_step(self.build, name)
            self.assertIn("--warn-is-error --shuffle 123", block)
            self.assertIn('--expected-sha "${{ github.sha }}"', block)
            self.assertIn('--run-id "${{ github.run_id }}"', block)
            self.assertIn('--run-attempt "${{ github.run_attempt }}"', block)
            self.assertIn('--job "${{ github.job }}"', block)

    def test_exact_commit_aggregation_requires_asan_and_tsan(self) -> None:
        aggregate = self.build[self.build.index("aggregate-test-stats:") :]
        aggregate = aggregate[: aggregate.index("report-ci-errors:")]
        self.assertIn("- build-linux-asan", aggregate)
        self.assertIn("- build-linux-tsan", aggregate)
        self.assertIn("--expected-lane linux-asan", aggregate)
        self.assertIn("--expected-lane linux-tsan", aggregate)
        self.assertIn("name: test-results-linux-asan", self.build)
        self.assertIn("name: test-results-linux-tsan", self.build)
        ratchet = json.loads(TEST_COUNT_RATCHET.read_text(encoding="utf-8"))
        self.assertIn("linux-asan", ratchet["lanes"])
        self.assertIn("linux-tsan", ratchet["lanes"])
        self.assertGreaterEqual(ratchet["lanes"]["linux-asan"]["minimumExecuted"], 1)
        self.assertGreaterEqual(ratchet["lanes"]["linux-tsan"]["minimumExecuted"], 1)
        expected_lanes = set(re.findall(r"--expected-lane ([A-Za-z0-9._-]+)", aggregate))
        self.assertEqual(expected_lanes, set(ratchet["lanes"]))

    def test_working_pushes_are_not_cancelled_before_evidence_finishes(self) -> None:
        self.assertIn("|| github.sha }}", self.build)
        self.assertIn(
            "cancel-in-progress: ${{ github.event_name == 'pull_request' }}",
            self.build,
        )

    def test_generated_documentation_runs_for_direct_pushes(self) -> None:
        block = named_step(self.build, "Verify all generated documentation and statistics")
        self.assertNotRegex(block, r"(?m)^\s+if:")

    def test_generated_metrics_publish_only_to_the_moving_state_tag(self) -> None:
        self.assertIn("STATE_REF: refs/tags/generated-repository-metrics", self.loc_counter)
        self.assertIn('"HEAD:${STATE_REF}"', self.loc_counter)
        self.assertIn("--force-with-lease=\"${STATE_REF}:${EXPECTED_REMOTE_OBJECT}\"", self.loc_counter)
        self.assertNotIn('"HEAD:refs/heads/Working"', self.loc_counter)
        self.assertNotIn("gh workflow run", self.loc_counter)
        self.assertIn("repository-metrics-source.json", self.loc_counter)

    def test_readme_ci_badge_tracks_fail_closed_aggregate_workflow(self) -> None:
        self.assertIn("[![Trusted exact-source CI]", self.readme)
        self.assertIn(
            "https://img.shields.io/github/checks-status/Krilliac/SparkEngine/Working"
            "?style=flat-square&label=CI",
            self.readme,
        )
        self.assertIn(
            "https://github.com/Krilliac/SparkEngine/actions/workflows/"
            "trusted-ci-aggregate.yml?query=branch%3AWorking",
            self.readme,
        )
        self.assertNotIn("[![Current commit checks]", self.readme)
        self.assertNotIn("github/check-runs", self.readme)
        self.assertNotIn("github/check-suites", self.readme)
        self.assertNotIn("trusted-ci-aggregate.yml/badge.svg", self.readme)

    def test_trusted_aggregate_commit_status_live_response_shapes(self) -> None:
        result = subprocess.run(
            ["node", ".github/scripts/test-trusted-ci-aggregate-status.js"],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_trusted_ci_badge_workflow_is_exact_and_fail_closed(self) -> None:
        aggregate = TRUSTED_CI_AGGREGATE_WORKFLOW.read_text(encoding="utf-8")
        header = aggregate[: aggregate.index("jobs:")]
        self.assertIn('name: "Trusted Exact-Source CI"', aggregate)
        self.assertIn("push:\n    branches: [Working]", header)
        self.assertIn(
            'workflows: ["CI-120 Trusted Verifier", "CodeQL Trusted Reporter"]',
            header,
        )
        self.assertIn("types: [in_progress, completed]", header)
        self.assertNotIn("workflow_dispatch:", header)
        self.assertIn("schedule:", header)
        self.assertNotIn("group: trusted-ci-aggregate", header)
        exact_job = yaml_section(aggregate, "exact-source", indent=2)
        job_concurrency = yaml_section(exact_job, "concurrency", indent=4)
        self.assertTrue(exact_field(job_concurrency, "group", "trusted-ci-aggregate", indent=6))
        self.assertTrue(exact_field(job_concurrency, "cancel-in-progress", "false", indent=6))
        self.assertEqual(aggregate.count("group: trusted-ci-aggregate"), 1)
        self.assertEqual(aggregate.count("actions: read"), 1)
        self.assertEqual(aggregate.count("contents: read"), 1)
        self.assertEqual(aggregate.count("statuses: write"), 1)
        self.assertEqual(aggregate.count("checks: write"), 1)
        self.assertNotRegex(aggregate, r"(?m)^\s+(?:actions|contents): write\s*$")
        self.assertIn("ref: Working", aggregate)
        self.assertIn("persist-credentials: false", aggregate)
        self.assertIn("github.workflow_sha", aggregate)
        self.assertIn("refs/heads/Working", aggregate)
        self.assertIn("verify-exact-required-gate.py", aggregate)
        self.assertIn("TARGET_SHA", aggregate)
        self.assertIn("GH_TOKEN", aggregate)
        self.assertIn("Upload exact trusted-CI receipt", aggregate)
        self.assertIn("Create pending exact-source badge check", aggregate)
        self.assertIn("Finalize exact-source badge check", aggregate)
        self.assertIn("Trusted Exact-Source CI Aggregate", aggregate)
        self.assertIn("Mark exact aggregate status pending", aggregate)
        self.assertIn("Finalize exact aggregate status", aggregate)
        self.assertIn("Trusted Exact-Source CI / Aggregate", aggregate)
        self.assertIn("trusted-ci-aggregate-status.js", aggregate)
        self.assertIn("getCombinedStatusForRef", aggregate)
        self.assertIn("listCommitStatusesForRef", aggregate)
        self.assertNotIn("status.sha", aggregate)
        finalize_status = named_step(aggregate, "Finalize exact aggregate status")
        self.assertIn(
            "if: always() && steps.aggregate-status.outputs.status-id != '' && "
            "(github.event_name != 'workflow_run' || github.event.action == 'completed')",
            finalize_status,
        )
        self.assertIn("conclusion: 'neutral'", aggregate)
        self.assertIn("exactSuccess && workingIsExact ? 'success' : 'failure'", aggregate)
        self.assertIn("conclusion,", aggregate)
        self.assertIn("per_page: 100", aggregate)
        self.assertIn("prior.status === 'completed'", aggregate)
        self.assertIn("['success', 'neutral'].includes(prior.conclusion)", aggregate)
        self.assertIn("github.event.action == 'completed'", aggregate)
        self.assertIn("app?.slug !== 'github-actions'", aggregate)
        self.assertIn("details_url", aggregate)
        self.assertIn("/runs/${check?.id}", aggregate)
        self.assertIn("String(check.details_url || '') !== `${detailsPrefix}${check.id}`", aggregate)
        self.assertNotIn("details_url: detailsUrl", aggregate)
        self.assertIn("trusted-ci-final-gate.env", aggregate)
        self.assertIn("listCommitStatusesForRef", aggregate)
        self.assertIn("ci120_status_id", aggregate)
        self.assertIn("codeql_status_id", aggregate)
        self.assertIn("currentBadge.status !== 'in_progress'", aggregate)
        self.assertIn("currentBadge.conclusion !== null", aggregate)
        self.assertNotIn("continue-on-error", aggregate)
        self.assertNotIn("|| true", aggregate)

    def test_site_data_accepts_only_exact_staged_build_and_writes_a_tag(self) -> None:
        self.assertEqual(self.site_data_publish.count("--staged-build-only"), 1)
        self.assertEqual(
            self.site_data_publish.count("verify-exact-required-gate.py"), 4
        )
        self.assertIn("Wait for trusted exact-commit CI evidence", self.site_data_publish)
        self.assertIn("SOURCE_RUN_ATTEMPT", self.site_data_publish)
        self.assertIn("STATE_REF: refs/tags/site-data", self.site_data_publish)
        self.assertIn('"HEAD:${STATE_REF}"', self.site_data_publish)
        self.assertIn(
            '--force-with-lease="${STATE_REF}:${EXPECTED_SITE_OBJECT}"',
            self.site_data_publish,
        )
        self.assertNotIn("HEAD:site-data", self.site_data_publish)
        self.assertNotIn("switch --create site-data", self.site_data_publish)
        self.assertEqual(
            self.site_data_publish.count("      statuses: read"),
            2,
            "both site-data jobs that invoke the exact gate need status read permission",
        )
        self.assertIn("id: exact-gate", self.site_data_publish)
        self.assertEqual(
            self.site_data_publish.count("exact_evidence.py write"), 1
        )
        self.assertEqual(
            self.site_data_publish.count("--exact-evidence-file"), 2
        )
        self.assertEqual(
            self.site_data_publish.count("--require-exact-evidence"), 3
        )
        self.assertIn("EXACT_CI120_STATUS_ID: ${{ steps.exact-gate.outputs.ci120_status_id }}", self.site_data_publish)
        self.assertIn("EXACT_CODEQL_STATUS_ID: ${{ steps.exact-gate.outputs.codeql_status_id }}", self.site_data_publish)
        publish_step = named_step(
            self.site_data_publish,
            "Recheck exact staged evidence, commit, and publish the site-data tag",
        )
        final_gate = publish_step.rindex("verify-exact-required-gate.py")
        exact_compare = publish_step.index("verify-gate-output", final_gate)
        state_compare = publish_step.index("REMOTE_OBJECT_NOW=", exact_compare)
        tag_push = publish_step.index('"HEAD:${STATE_REF}"', state_compare)
        self.assertLess(final_gate, exact_compare)
        self.assertLess(exact_compare, state_compare)
        self.assertLess(state_compare, tag_push)
        self.assertNotIn("continue-on-error", publish_step)
        self.assertNotIn("|| true", publish_step)

    def test_release_controller_cannot_run_from_a_caller_selected_ref(self) -> None:
        header = self.release[: self.release.index("permissions:")]
        self.assertIn("repository_dispatch:", header)
        self.assertNotIn("workflow_dispatch:", header)
        self.assertNotIn("  push:", header)
        controller = named_step(self.release, "Attest current trusted release controller")
        self.assertIn('EVENT_REF" != "refs/heads/Working', controller)
        self.assertIn('LOCAL_SHA" != "$WORKFLOW_SHA', controller)
        self.assertIn('LOCAL_SHA" != "$REMOTE_SHA', controller)

    def test_release_concurrency_uses_only_supported_github_schema(self) -> None:
        release_job = self.release[self.release.index("  release:\n") :]
        concurrency = release_job[
            release_job.index("    concurrency:\n") : release_job.index("\n    steps:\n")
        ]
        self.assertIn("      group: sparkengine-publication-global", concurrency)
        self.assertIn("      cancel-in-progress: false", concurrency)
        self.assertNotIn("queue:", concurrency)

    def test_all_workflow_concurrency_uses_supported_github_schema(self) -> None:
        offenders = []
        for path in sorted((REPO_ROOT / ".github" / "workflows").glob("*.yml")):
            if re.search(r"(?m)^\s+queue:\s*", path.read_text(encoding="utf-8")):
                offenders.append(path.name)
        self.assertEqual(offenders, [])

    def test_versioned_release_cannot_publish_debug_without_release(self) -> None:
        matrix = named_step(self.release, "Determine build configurations")
        self.assertIn(
            'if [[ "$IS_VERSIONED" == "true" && "$REQUESTED_CONFIGS" == "debug" ]]',
            matrix,
        )
        self.assertIn("versioned releases must include Release artifacts", matrix)

    def test_release_binaries_bind_and_verify_the_requested_cmake_version(self) -> None:
        installer_cmake = (REPO_ROOT / "SparkInstaller" / "CMakeLists.txt").read_text(encoding="utf-8")
        launcher_cmake = (REPO_ROOT / "SparkLauncher" / "CMakeLists.txt").read_text(encoding="utf-8")
        installer_header = (REPO_ROOT / "SparkInstaller" / "src" / "Installer.h").read_text(encoding="utf-8")
        launcher_main = (REPO_ROOT / "SparkLauncher" / "src" / "main.cpp").read_text(encoding="utf-8")
        launch_step = named_step(self.release, "Launch staged executable")

        self.assertIn('SPARK_INSTALLER_VERSION=\\"${PROJECT_VERSION}\\"', installer_cmake)
        self.assertIn('SPARK_LAUNCHER_VERSION=\\"${PROJECT_VERSION}\\"', launcher_cmake)
        self.assertIn("SPARK_INSTALLER_VERSION", installer_header)
        self.assertNotIn('kInstallerVersion = "1.0.0"', installer_header)
        self.assertIn("SPARK_LAUNCHER_VERSION", launcher_main)
        self.assertNotIn('SparkLauncher 1.0.0', launcher_main)
        self.assertIn('EXPECTED_VERSION: ${{ needs.prepare.outputs.cmake_version }}', launch_step)
        self.assertIn('EXPECTED_PLATFORM: ${{ matrix.platform_name }}', launch_step)

        command_match = re.search(
            r'(?m)^\s+((?=[^\n]*grep)(?=[^\n]*installer-version[.]txt)[^\n]+)$',
            launch_step,
        )
        self.assertIsNotNone(command_match, "staged installer version gate is missing")
        self.assertIn(
            "sed $'s/\\r$//' installer-version.txt |",
            command_match.group(1),
            "staged installer version gate must normalize only a trailing CR before exact comparison",
        )
        command = command_match.group(1).replace("installer-version.txt", "-")
        if os.name == "nt":
            bash = Path(os.environ["ProgramFiles"]) / "Git" / "bin" / "bash.exe"
        else:
            bash = Path(shutil.which("bash") or "")
        self.assertTrue(bash.is_file(), f"bash is unavailable: {bash}")
        environment = dict(os.environ)
        environment.update(EXPECTED_VERSION="7.8.9", EXPECTED_PLATFORM="Linux")
        for displayed, expected_status in (
            ("SparkInstaller 7.8.9 (Linux)\n", 0),
            ("SparkInstaller 7.8.9 (Linux)\r\n", 0),
            ("SparkInstaller 7x8y9 (Linux)\n", 1),
            ("SparkInstaller 7.8.9 (Windows)\n", 1),
        ):
            with self.subTest(displayed=displayed.rstrip()):
                completed = subprocess.run(
                    [str(bash), "-c", command],
                    input=displayed,
                    text=True,
                    env=environment,
                    check=False,
                    capture_output=True,
                )
                self.assertEqual(completed.returncode, expected_status)

        self.assertIn(
            'SPARK_EXPECTED_VERSION_OUTPUT=SparkInstaller ${PROJECT_VERSION} (${_sparkinstaller_platform})',
            installer_cmake,
        )
        self.assertIn(
            'SPARK_EXPECTED_VERSION_OUTPUT=SparkLauncher ${PROJECT_VERSION}',
            launcher_cmake,
        )
        self.assertIn(
            'add_test(NAME SparkInstallerVersion\n        COMMAND "${CMAKE_COMMAND}"',
            installer_cmake,
        )
        for cmake_file in (installer_cmake, launcher_cmake):
            self.assertIn(
                'if(NOT _spark_version_output STREQUAL "${SPARK_EXPECTED_VERSION_OUTPUT}\\n")',
                cmake_file,
            )

    def test_late_release_failure_redrafts_the_exact_durable_release(self) -> None:
        recovery = named_step(self.release, "Recover incomplete public release")
        self.assertIn("      if: failure()", recovery)
        self.assertIn("recover_release_publication.py", recovery)
        self.assertIn(
            'STATE_FILE="$GITHUB_WORKSPACE/badge-repository/.github/badges/downloads-data.json"',
            recovery,
        )
        self.assertIn('--state-file "$STATE_FILE"', recovery)
        self.assertIn('--run-id "$GITHUB_RUN_ID"', recovery)
        self.assertIn('--run-attempt "$GITHUB_RUN_ATTEMPT"', recovery)
        self.assertIn('--source-sha "$GITHUB_SHA"', recovery)
        self.assertNotIn("--release-id", recovery)
        self.assertNotIn("steps.release-freeze.outputs.target_release_id", recovery)

    def test_failed_or_cancelled_release_has_independent_durable_recovery(self) -> None:
        recovery = RELEASE_RECOVERY_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("workflow_run:", recovery)
        self.assertIn('workflows: ["Publish Builds"]', recovery)
        self.assertIn("types: [completed]", recovery)
        self.assertIn("github.event.workflow_run.conclusion != 'success'", recovery)
        self.assertIn("contents: write", recovery)
        self.assertIn("ref: Working", recovery)
        self.assertIn("persist-credentials: false", recovery)
        self.assertIn("refs/tags/generated-release-counters", recovery)
        self.assertIn("recover_release_publication.py", recovery)
        self.assertIn('--state-file "$RUNNER_TEMP/downloads-data.json"', recovery)
        self.assertIn('--run-id "$SOURCE_RUN_ID"', recovery)
        self.assertIn('--run-attempt "$SOURCE_RUN_ATTEMPT"', recovery)
        self.assertIn('--source-sha "$SOURCE_SHA"', recovery)


if __name__ == "__main__":
    unittest.main(verbosity=2)
