#!/usr/bin/env python3
"""Contract tests for fail-closed required workflow execution."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"


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


class WorkflowFailurePropagationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = BUILD_WORKFLOW.read_text(encoding="utf-8")
        cls.release = RELEASE_WORKFLOW.read_text(encoding="utf-8")

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

    def test_sanitizer_runner_receives_runtime_log_prefix(self) -> None:
        self.assertIn("--runtime-log-prefix build/asan-runtime", self.build)
        self.assertIn("--runtime-log-prefix build/tsan-runtime", self.build)

    def test_msan_does_not_receive_runtime_log_prefix(self) -> None:
        msan_block = named_step(self.build, "Run Tests under MSan")
        self.assertNotIn("--runtime-log-prefix", msan_block)

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
