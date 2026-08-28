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
        gate = yaml_section(workflow, "required-ci-gate", indent=2)
    except AssertionError as exc:
        errors.append(str(exc))
        gate = ""
    if gate:
        if not exact_field(gate, "if", "always()"):
            errors.append("required-ci-gate must run under exact if: always()")
        for dependency in ("build-linux-asan", "build-linux-tsan", "aggregate-test-stats"):
            if len(re.findall(rf"(?m)^      - {dependency}$", gate)) != 1:
                errors.append(f"required-ci-gate must need {dependency} exactly once")
    return errors


class WorkflowFailurePropagationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = BUILD_WORKFLOW.read_text(encoding="utf-8")
        cls.release = RELEASE_WORKFLOW.read_text(encoding="utf-8")

    def test_required_workflow_semantics_are_fail_closed(self) -> None:
        self.assertEqual(required_workflow_errors(self.build), [])

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
