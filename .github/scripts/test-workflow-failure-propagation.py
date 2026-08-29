#!/usr/bin/env python3
"""Contract tests for fail-closed required workflow execution."""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"
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
        badge_checkout = named_step(workflow, "Checkout canonical badge branch")
    except AssertionError as error:
        errors.append(str(error))
    else:
        ordered_step_names = [name for name, _block in step_blocks(workflow)]
        required_ci_position = ordered_step_names.index(
            "Verify exact source commit passed Required CI Gate"
        )
        profile_gate_position = ordered_step_names.index(step_name)
        badge_checkout_position = ordered_step_names.index("Checkout canonical badge branch")
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
        for dependency in ("build-linux-asan", "build-linux-tsan", "telemetry-integration", "aggregate-test-stats"):
            if len(re.findall(rf"(?m)^      - {dependency}$", gate)) != 1:
                errors.append(f"required-ci-gate must need {dependency} exactly once")
        try:
            verifier = named_step(gate, "Verify every required job succeeded")
        except AssertionError as exc:
            errors.append(str(exc))
        else:
            if re.search(r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", verifier):
                errors.append("required-ci-gate verifier has a conditional/error bypass")
            if verifier.count("env:") != 1 or verifier.count("NEEDS_JSON: ${{ toJSON(needs) }}") != 1:
                errors.append("required-ci-gate verifier must consume exact needs JSON once")
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
        cls.tests_cmake = TESTS_CMAKE.read_text(encoding="utf-8")

    def test_required_workflow_semantics_are_fail_closed(self) -> None:
        self.assertEqual(required_workflow_errors(self.build), [])

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
            "        run: python3 .github/scripts/verify-required-jobs.py",
            "          NEEDS_JSON: '{}'\n"
            "        run: python3 .github/scripts/verify-required-jobs.py",
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
        tag_binding = named_step(self.release, "Bind release tag to workflow commit")
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
