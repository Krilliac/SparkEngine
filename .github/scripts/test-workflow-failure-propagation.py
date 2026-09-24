#!/usr/bin/env python3
"""Contract tests for fail-closed required workflow execution."""

from __future__ import annotations

import copy
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock
from pathlib import Path

import yaml


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
RELEASE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release.yml"
RELEASE_RECOVERY_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "release-recovery.yml"
LOC_COUNTER_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "loc-counter.yml"
SITE_DATA_PUBLISH_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "site-data-publish.yml"
TRUSTED_CI_AGGREGATE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "trusted-ci-aggregate.yml"
CHECK_FORMAT_SCRIPT = REPO_ROOT / ".github" / "scripts" / "check-format-changed.sh"
CHECK_FORMAT_COMMAND = "bash .github/scripts/check-format-changed.sh"
CHECK_FORMAT_TEST_COMMAND = "bash .github/scripts/test-check-format-changed.sh"
README = REPO_ROOT / "README.md"
TEST_COUNT_RATCHET = REPO_ROOT / ".github" / "test-count-ratchet.json"
CMAKE_ROOT = REPO_ROOT / "CMakeLists.txt"
BUILD_IMGUI_CMAKE = REPO_ROOT / "cmake" / "BuildImGui.cmake"
TEMPLATE_VERIFIER = REPO_ROOT / "cmake" / "VerifyInstalledTemplates.cmake"
TEMPLATE_RUNTIME_HEADER = REPO_ROOT / "SparkEngine" / "Source" / "Game" / "TemplateRuntime.h"
FPS_TEMPLATE_HEADER = REPO_ROOT / "Templates" / "FPSStarter" / "Source" / "GameModule.h"
TESTS_CMAKE = REPO_ROOT / "Tests" / "CMakeLists.txt"
TEST_TELEMETRY_SPOOL = REPO_ROOT / "Tests" / "TestTelemetrySpool.cpp"
TELEMETRY_EXPECTED_COUNT = 8
REQUIRED_CI_JOBS = (
    "fuzz-policy",
    "validate-ci-tools",
    "performance-budget-governance",
    "check-format",
    "validate-prompts",
    "validate-ops100",
    "check-thirdparty-manifest",
    "check-supply-chain",
    "license-compliance",
    "build-linux-asan",
    "build-linux-tsan",
    "telemetry-integration",
    "build-windows-vs2022",
    "module-profile-lifecycle",
    "build-windows-shipping",
    "module-profile-package-smoke",
    "build-linux-gcc",
    "build-linux-clang",
    "coverage",
    "clang-tidy",
    "todo-count",
    "build-installer",
    "aggregate-test-stats",
    "module-evidence",
)
REQUIRED_CI_JOBS_JSON = json.dumps(REQUIRED_CI_JOBS, separators=(",", ":"))
MINGW_WINE_JOB = "build-linux-mingw-wine"

# Every check tools/validate-all.sh runs must make CI red when it fails. Each
# maps to the required job and the exact run line that invokes it (or its
# direct equivalent: the .sh wrapper's exec target, or the CMake target that
# runs the same checker). A check may instead be advisory only with a reason.
VALIDATE_ALL = REPO_ROOT / "tools" / "validate-all.sh"
VALIDATE_ALL_REQUIRED_INVOCATIONS = {
    "check-pragma-once.sh": ("validate-ci-tools", "bash tools/check-pragma-once.sh"),
    "check-editor-panels.sh": ("validate-ci-tools", "bash tools/check-editor-panels.sh"),
    "check-test-registration.sh": ("validate-ci-tools", "bash tools/check-test-registration.sh"),
    "check-deprecated-submodules.sh": ("validate-ci-tools", "bash tools/check-deprecated-submodules.sh"),
    "check-thirdparty-manifest-sync.sh": (
        "check-thirdparty-manifest",
        "./tools/check-thirdparty-manifest-sync.sh --ci",
    ),
    # check-supply-chain.sh only execs this checker.
    "check-supply-chain.sh": ("check-supply-chain", "python3 tools/check-supply-chain.py"),
    # cmake/SparkFuzzPolicy.cmake runs check_fuzz_policy.py --ci, as the .sh does.
    "check-fuzz-policy.sh": (
        "fuzz-policy",
        "cmake --build build/fuzz-policy --target check-fuzz-policy",
    ),
    "check-wiki-nav.sh": ("validate-ci-tools", "bash tools/check-wiki-nav.sh"),
    "check-wiring.sh": ("validate-ci-tools", "bash tools/check-wiring.sh"),
    "check-doxygen-coverage.sh": ("validate-ci-tools", "bash tools/check-doxygen-coverage.sh check"),
    "check-cross-utilization.sh": ("validate-ci-tools", "bash tools/check-cross-utilization.sh"),
    "check-di-singletons.sh": ("validate-ci-tools", "bash tools/check-di-singletons.sh"),
    # The non-gated .sh runs this declarative validation plus the adversarial
    # unit tests, which validate-ci-tools also runs as its own step.
    "check-module-evidence.sh": (
        "validate-ci-tools",
        "python3 tools/module-evidence/validate_manifest.py --manifest tools/module-evidence/manifest.json"
        " --repo-root . --policy-only",
    ),
}
VALIDATE_ALL_ADVISORY_CHECKS = {
    "check-bloat.sh": "size thresholds are guidance (CLAUDE.md); new-only mode is red on files awaiting review",
    "check-wiki-quality.sh": "validate-all runs it with --warn-only by design",
}

CLANG_TIDY_SOURCE_ROOTS = (
    "SparkEngine/Source",
    "SparkEditor/Source",
    "SparkConsole/src",
    "SparkDaemon/src",
    "SparkGateway/src",
    "SparkLauncher/src",
    "SparkServer/src",
    "SparkWorker/src",
    "SparkCooker/src",
    "SparkAutomation/src",
    "SparkBuild/src",
    "SparkInstaller/src",
    "SparkShaderCompiler/src",
    "GameModules",
)

# Every failure suppression (``|| true``, ``|| :``, ``|| echo``, ``|| exit 0``)
# inside a required job, keyed by exact (job, step, stripped run line). Only
# cache statistics, cache hygiene, fallback selection, and failure-only
# reporting lines belong here. A suppression on a line that produces gate
# evidence (a scan, test, count, or validator) must be fixed, never listed.
REVIEWED_REQUIRED_JOB_SUPPRESSIONS = frozenset({
    ("check-format", "Extract check-format error summary", "check-format-output.log || true"),
    ("build-linux-asan", "Configure CMake (ASan + UBSan + LSan)", 'command -v ccache >/dev/null && ccache --zero-stats || echo "::warning::ccache not installed, proceeding without cache"'),
    ("build-linux-asan", "Print ccache stats", 'command -v ccache >/dev/null && ccache --show-stats || echo "::warning::ccache not installed, skipping stats"'),
    ("build-linux-tsan", "Configure CMake (TSan)", 'command -v ccache >/dev/null && ccache --zero-stats || echo "::warning::ccache not installed, proceeding without cache"'),
    ("build-linux-tsan", "Print ccache stats", 'command -v ccache >/dev/null && ccache --show-stats || echo "::warning::ccache not installed, skipping stats"'),
    ("build-windows-vs2022", "Print sccache stats", 'sccache --show-stats 2>&1 | tee sccache-stats.txt || echo "::warning::sccache --show-stats failed"'),
    ("build-windows-vs2022", "Print sccache stats", 'sccache --stop-server || echo "::warning::sccache --stop-server failed"'),
    ("build-linux-gcc", "Configure CMake", 'command -v ccache >/dev/null && ccache --zero-stats || echo "::warning::ccache not installed, proceeding without cache"'),
    ("build-linux-gcc", "Build all targets", "find build -name '*.pch' -delete 2>/dev/null || true"),
    ("build-linux-gcc", "Print ccache stats", 'command -v ccache >/dev/null && ccache --show-stats || echo "::warning::ccache not installed, skipping stats"'),
    ("build-linux-clang", "Configure CMake", 'command -v ccache >/dev/null && ccache --zero-stats || echo "::warning::ccache not installed, proceeding without cache"'),
    ("build-linux-clang", "Build all targets", "find build -name '*.pch' -delete 2>/dev/null || true"),
    ("build-linux-clang", "Build all targets", "find build -name 'lib*.a' -delete 2>/dev/null || true"),
    ("build-linux-clang", "Print ccache stats", 'command -v ccache >/dev/null && ccache --show-stats || echo "::warning::ccache not installed, skipping stats"'),
    ("coverage", "Configure", 'command -v ccache >/dev/null && ccache --zero-stats || echo "::warning::ccache not installed, proceeding without cache"'),
    ("coverage", "Print ccache stats", 'command -v ccache >/dev/null && ccache --show-stats || echo "::warning::ccache not installed, skipping stats"'),
    ("clang-tidy", "Run clang-tidy", "warn_count=$(grep -cE ':\\s*(warning|error):' clang-tidy-output.log || true)"),
    ("clang-tidy", "Extract clang-tidy error summary", "clang-tidy-output.log || true"),
})
FAILURE_SUPPRESSION = re.compile(r"\|\|\s*(?:true\b|:(?=\s|$|\))|echo\b|exit\s+0\b)")

# C/C++ suffixes check-format must route to clang-format. Objective-C++
# (.mm) is Metal-only platform code and is deliberately outside the gate.
FORMAT_ROOTS = (
    "SparkEngine/Source", "GameModules", "SparkEditor/Source", "SparkConsole/src",
    "SparkShaderCompiler/src", "SparkBuild/src", "SparkInstaller/src", "SparkDaemon/src",
    "SparkServer/src", "SparkGateway/src", "SparkCooker/src", "SparkWorker/src",
    "SparkAutomation/src", "SparkLauncher/src", "Tests",
)
CXX_SOURCE_SUFFIXES = frozenset({".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".c", ".cc", ".cpp", ".cxx"})

sys.path.insert(0, str(REPO_ROOT / "Tools"))

from buildmatrix.workflow import WorkflowError, parse_workflow_yaml  # noqa: E402


def bash_executable() -> str:
    """Resolve Bash for release-workflow fixtures on GitHub and Windows hosts."""

    if os.name == "nt":
        # ``bash`` on PATH can be the WindowsApps WSL launcher. Its cwd and
        # environment forwarding differ from the Windows Git Bash shell used
        # by the release workflow, so prefer the actual Git installation.
        git_bash = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe"
        if git_bash.is_file():
            return str(git_bash)
    found = shutil.which("bash")
    if found:
        if os.name == "nt" and any(part.casefold() == "windowsapps" for part in Path(found).parts):
            raise FileNotFoundError(
                "Git Bash is required for release-workflow fixtures; the WindowsApps WSL launcher is incompatible"
            )
        return found
    common = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe"
    if common.is_file():
        return str(common)
    raise FileNotFoundError("bash is required for release-workflow fixture execution")


def local_release_fixture_script(script: str) -> str:
    """Bind the Ubuntu-only ``python3`` metadata probe to the test interpreter on Windows."""

    if os.name != "nt":
        return script
    command = "python3 - "
    if script.count(command) != 1:
        raise AssertionError("release metadata fixture must contain one python3 probe")
    interpreter = shlex.quote(Path(sys.executable).as_posix())
    return script.replace(command, f"{interpreter} - ", 1)


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


def aggregate_failure_terminalization_errors(workflow: str) -> list[str]:
    """Require authenticated failure publication before the aggregate action fails."""

    try:
        finalize = named_step(workflow, "Finalize exact aggregate status")
    except AssertionError as error:
        return [str(error)]

    failure_start = finalize.find("if (state !== 'success') {")
    failure_end = finalize.find("\n          const exactEvidence", failure_start)
    if failure_start < 0 or failure_end < 0:
        return ["aggregate failure branch is missing or unbounded"]
    failure = finalize[failure_start:failure_end]
    required = {
        "pending identity validation": "statusContract.validateLatestPendingStatus({",
        "authenticated failure publication": "state: 'failure'",
        "failure description": "failureDescription,",
        "post-publication combined inventory": "getCombinedStatusForRef({",
        "post-publication listed inventory": "listCommitStatusesForRef({",
        "exact published failure id": "postFailureMatches[0].id !== publishedFailure.id",
        "exact published failure state": "postFailureMatches[0].state !== 'failure'",
        "listed failure state": "postFailureListed.state !== 'failure'",
        "authenticated bot login": "postFailureListed.creator?.login !== 'github-actions[bot]'",
        "authenticated bot id": "postFailureListed.creator?.id !== 41898282",
        "post-publication rejection": "Post-publication aggregate failure identity changed.",
        "action failure": "core.setFailed(`Trusted exact-source aggregate status failed:",
    }
    errors = [label for label, marker in required.items() if failure.count(marker) != 1]
    if errors:
        return errors

    ordered = [
        "statusContract.validateLatestPendingStatus({",
        "const publishedFailure = await statusContract.publishValidatedTerminalStatus({",
        "const [postFailureCombined, postFailureListedResponse] = await Promise.all([",
        "Post-publication aggregate failure identity changed.",
        "core.setFailed(`Trusted exact-source aggregate status failed:",
    ]
    positions = [failure.index(marker) for marker in ordered]
    if positions != sorted(positions) or len(set(positions)) != len(positions):
        errors.append("aggregate failure publication is not authenticated and verified before failure")
    return errors


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


def run_command(block: str, *, indent: int = 4) -> str | None:
    """Return a step's ``run:`` command folded back into a single line.

    A folded (``>-``) scalar is the same command as an inline one, but a literal
    comparison cannot see that: when the readiness gate grew a second flag and
    moved to ``run: >-``, every exact check of it silently started comparing
    against the string ``>-`` instead of against the command. Folding here keeps
    the comparison exact without pinning the YAML style the command is written
    in. Returns None when the step does not carry exactly one ``run:``.
    """

    pattern = re.compile(rf"^{' ' * indent}run:[ \t]*(.*?)[ \t]*$", re.MULTILINE)
    matches = list(pattern.finditer(block))
    if len(matches) != 1:
        return None
    head = matches[0].group(1)
    if head not in {">", ">-", ">+", "|", "|-", "|+"}:
        return head
    folded: list[str] = []
    for line in block[matches[0].end():].split("\n")[1:]:
        if not line.strip():
            break
        if len(line) - len(line.lstrip(" ")) <= indent:
            break
        folded.append(line.strip())
    return " ".join(folded)


def suppress_run_command(step: str) -> str:
    """Append a failure-swallowing suffix to the step's command, whatever its style."""

    lines = step.rstrip("\n").split("\n")
    lines[-1] = f"{lines[-1]} || echo ignored"
    return "\n".join(lines) + step[len(step.rstrip("\n")):]


def inject_before_run(step: str, injected: str) -> str:
    """Insert a sibling key immediately above the step's ``run:`` key."""

    lines = step.split("\n")
    for index, line in enumerate(lines):
        if line.strip().startswith("run:"):
            lines.insert(index, injected)
            break
    else:
        raise AssertionError("step has no run: key to inject above")
    return "\n".join(lines)


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
    step_name = "Verify stable-v1 candidate is qualified for versioned publication"
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
    # The v0.9 bootstrap and ordinary v1 qualification are distinct stages.
    # Pin both exact validators and the version branch so neither path can be
    # weakened by a waiver, a swapped selector, or a successful no-op.
    expected_readiness = " ".join((
        'if [[ "${{ needs.prepare.outputs.version }}" == "0.9.0" ]]; then',
        "python3 tools/site-data/validate.py --require-predecessor-candidate",
        "else",
        "python3 tools/site-data/validate.py --require-candidate-ready",
        "fi",
    ))
    if run_command(readiness, indent=6) != expected_readiness:
        errors.append("stable-v1 publication gate must run the exact stage-specific readiness validators")
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


def release_acceptance_gate_errors(workflow: str) -> list[str]:
    """Require both final publication paths to use the fail-closed acceptance gate."""

    errors: list[str] = []
    stable_publish_step: str | None = None
    for step_name in (
        "Publish complete stable versioned release",
        "Publish complete nightly rolling release",
    ):
        try:
            step = named_step(workflow, step_name)
        except AssertionError as error:
            errors.append(str(error))
            continue
        if step_name == "Publish complete stable versioned release":
            stable_publish_step = step
        for fragment in (
            "GITHUB_REPOSITORY: ${{ github.repository }}",
            "IS_VERSIONED: ${{ needs.prepare.outputs.is_versioned }}",
            "EXPECTED_ASSETS_FILE: expected-release-assets.txt",
            "EXPECTED_DIGESTS_FILE: expected-release-digests.txt",
            'python3 -I "$GITHUB_WORKSPACE/.github/scripts/release-acceptance-gate.py"',
            "verify-exact-required-gate.py",
            "verify-release-publication-boundary.sh",
        ):
            if fragment not in step:
                errors.append(f"{step_name} is missing required acceptance-gate fragment: {fragment}")
        if step_name == "Publish complete stable versioned release":
            if "recover_release_publication.py" in step or "report_failed_immutable_publication" not in step:
                errors.append("stable publication must report immutable failure without redrafting")
        elif "recover_release_publication.py" not in step:
            errors.append("nightly publication must retain mutable redraft recovery")
        if "gh api --method PATCH" in step:
            errors.append(f"{step_name} must not publish through a bare gh API PATCH")
    if stable_publish_step is not None:
        readiness_check = 'python3 "$GITHUB_WORKSPACE/tools/site-data/validate.py" --require-candidate-ready'
        acceptance_command = 'python3 -I "$GITHUB_WORKSPACE/.github/scripts/release-acceptance-gate.py"'
        if readiness_check not in stable_publish_step:
            errors.append("stable publication must revalidate readiness immediately before acceptance")
        elif (
            acceptance_command in stable_publish_step
            and stable_publish_step.index(readiness_check)
            > stable_publish_step.index(acceptance_command)
        ):
            errors.append("stable publication readiness recheck must precede acceptance PATCH")
    return errors


def release_acceptance_recovery_errors(workflow: str) -> list[str]:
    """Ensure compensation is armed only after a PATCH can have started."""

    errors: list[str] = []
    acceptance_command = 'python3 -I "$GITHUB_WORKSPACE/.github/scripts/release-acceptance-gate.py"'
    marker = "RELEASE_ACCEPTANCE_PATCH_STARTED_FILE"
    for step_name in (
        "Publish complete stable versioned release",
        "Publish complete nightly rolling release",
    ):
        try:
            step = named_step(workflow, step_name)
        except AssertionError as error:
            errors.append(str(error))
            continue
        required_fragments = (
            f'{marker}="$RUNNER_TEMP/release-acceptance-patch-started-$RELEASE_ID"',
            f'rm -f "${marker}"',
            f"export {marker}",
            f'if [[ "$publication_attempted" != "true" && ! -f "${marker}" ]]; then',
        )
        for fragment in required_fragments:
            if fragment not in step:
                errors.append(f"{step_name} is missing PATCH-attempt recovery guard: {fragment}")
        marker_setup = step.find(f"{marker}=")
        handler = "report_failed_immutable_publication" if step_name == "Publish complete stable versioned release" else "redraft_failed_publication"
        trap_setup = step.find(f"trap {handler} ERR")
        command_position = step.find(acceptance_command)
        attempt_position = step.find("publication_attempted=true")
        if marker_setup < 0 or trap_setup < 0 or marker_setup > trap_setup:
            errors.append(f"{step_name} arms recovery before its marker is reset")
        if command_position < 0 or attempt_position < command_position:
            errors.append(f"{step_name} arms unconditional recovery before acceptance PATCH dispatch")
        if f'trap - ERR\n        rm -f "${marker}"' not in step:
            errors.append(f"{step_name} does not clear its PATCH-attempt marker after recovery is disarmed")
    return errors


CANONICAL_RELEASE_SBOM = "SparkEngine-SBOM.spdx.json"
SBOM_ARGUMENT_RE = re.compile(r"""--sbom[ =]+("[^"]*"|'[^']*'|\S+)""")


def release_sbom_name_errors(workflow: str) -> list[str]:
    """Every consumer of the release SBOM must read the file sbom-action wrote.

    The stable bundle verifiers, the signing tests and
    verify_published_stable_release.py consume the published asset
    CANONICAL_RELEASE_SBOM, so the anchore/sbom-action output-file must be that
    name, and every ``--sbom`` argument and anchore/scan-action ``sbom`` input
    in release.yml must name the same file (optionally under
    $GITHUB_WORKSPACE).
    """

    try:
        document = parse_workflow_yaml(workflow)
    except WorkflowError as error:
        return [f"release workflow is not safely parseable: {error}"]
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        return ["release workflow must define a jobs mapping"]

    def normalized(value: str) -> str:
        value = value.strip().strip("\"'")
        for prefix in ("$GITHUB_WORKSPACE/", "${GITHUB_WORKSPACE}/", "${{ github.workspace }}/"):
            if value.startswith(prefix):
                return value[len(prefix):]
        return value

    errors: list[str] = []
    outputs: list[str] = []
    consumers: list[tuple[str, str]] = []
    for job_id, job in jobs.items():
        steps = job.get("steps") if isinstance(job, dict) else None
        for step in steps if isinstance(steps, list) else []:
            if not isinstance(step, dict):
                continue
            label = f"{job_id}: {step.get('name', step.get('uses', '<unnamed step>'))}"
            uses = str(step.get("uses", ""))
            inputs = step.get("with") if isinstance(step.get("with"), dict) else {}
            if uses.startswith("anchore/sbom-action@"):
                outputs.append(str(inputs.get("output-file", "")))
            if uses.startswith("anchore/scan-action@") and "sbom" in inputs:
                consumers.append((label, normalized(str(inputs["sbom"]))))
            run = step.get("run")
            if isinstance(run, str):
                for match in SBOM_ARGUMENT_RE.finditer(run):
                    consumers.append((label, normalized(match.group(1))))

    if outputs != [CANONICAL_RELEASE_SBOM]:
        errors.append(
            f"release.yml must generate exactly one SBOM with output-file {CANONICAL_RELEASE_SBOM}, found {outputs}"
        )
    if not consumers:
        errors.append("release.yml has no SBOM consumer (--sbom argument or scan-action sbom input)")
    for label, name in consumers:
        if name != CANONICAL_RELEASE_SBOM:
            errors.append(f"{label} reads SBOM {name!r}, but sbom-action writes {CANONICAL_RELEASE_SBOM!r}")
    return errors


def release_package_gate_errors(workflow: str) -> list[str]:
    """Reject advisory or error-suppressing release-package validation paths."""

    try:
        document = parse_workflow_yaml(workflow)
    except WorkflowError as error:
        return [f"release package workflow is not safely parseable: {error}"]

    errors: list[str] = []
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        return ["release package workflow must define a jobs mapping"]
    if "defaults" in document:
        errors.append("release package workflow must not use workflow-wide defaults")

    required_steps = {
        "Validate external package consumption": (
            "cmake --install",
            "ctest --test-dir smoke-build",
            "--no-tests=error",
        ),
        "Build every template against the installed SDK": ("VerifyInstalledTemplates.cmake",),
        "Validate staged package executable BOM": ("ValidateStagedPackageExecutables.cmake",),
        "Generate CPack packages": ("cpack",),
        "Extract and smoke-test portable package": (
            "validate-extracted-package.py --preflight-archive",
            "--package-root",
            "ctest --test-dir package-smoke-build",
            "--no-tests=error",
            "VerifyInstalledTemplates.cmake",
        ),
    }

    for job_name, expected_shell in (
        ("build-windows", "pwsh"),
        ("build-linux", None),
        ("build-macos", None),
    ):
        job = jobs.get(job_name)
        if not isinstance(job, dict):
            errors.append(f"{job_name} is missing or not a mapping")
            continue
        for field in ("if", "continue-on-error", "defaults"):
            if field in job:
                errors.append(f"{job_name} package gate must not define {field}")

        steps = job.get("steps")
        if not isinstance(steps, list):
            errors.append(f"{job_name} package gate must define a steps sequence")
            continue

        for step_name, fragments in required_steps.items():
            matches = [
                step
                for step in steps
                if isinstance(step, dict) and step.get("name") == step_name
            ]
            if len(matches) != 1:
                errors.append(f"{job_name} must define {step_name!r} exactly once")
                continue
            step = matches[0]
            for field in ("if", "continue-on-error", "defaults"):
                if field in step:
                    errors.append(f"{job_name}/{step_name} must not define {field}")
            if expected_shell is None:
                if "shell" in step:
                    errors.append(f"{job_name}/{step_name} must use the runner default shell")
            elif step.get("shell") != expected_shell:
                errors.append(
                    f"{job_name}/{step_name} must use exact shell: {expected_shell}"
                )

            run = step.get("run")
            if not isinstance(run, str):
                errors.append(f"{job_name}/{step_name} must have a runnable command block")
                continue
            for fragment in fragments:
                if fragment not in run:
                    errors.append(f"{job_name}/{step_name} is missing {fragment!r}")
            if re.search(r"\|\|\s*(?:true|:)\b", run):
                errors.append(f"{job_name}/{step_name} suppresses a package-gate failure")
            if expected_shell is None and re.search(
                r"(?mi)^\s*set\s+\+(?:e\b|o\s+(?:errexit|pipefail)\b)", run
            ):
                errors.append(f"{job_name}/{step_name} disables POSIX error propagation")

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
        restore_step_names = {
            "build-windows-vs2022": "Restore sccache cache",
            "build-windows-vs2026": "Restore sccache cache",
        }
        restore_name = restore_step_names.get(job_name, "Restore build directory")
        required_names = (
            restore_name,
            "Scrub restored test evidence",
            "Validate and summarize test statistics",
            "Upload machine-readable test results",
        )
        try:
            positions = {name: ordered_names.index(name) for name in required_names}
        except ValueError as error:
            errors.append(f"{job_name} test-evidence steps are incomplete: {error}")
            continue
        if positions["Scrub restored test evidence"] != positions[restore_name] + 1:
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
                "--minimum-tests 6900",
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
                "--minimum-tests 6900",
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
        expected_dependencies = list(REQUIRED_CI_JOBS)
        needs_match = re.search(
            r"(?ms)^    needs:\n(?P<body>(?:      - [a-z0-9-]+\n)+)", gate
        )
        actual_dependencies = (
            re.findall(r"(?m)^      - ([a-z0-9-]+)$", needs_match.group("body"))
            if needs_match else []
        )
        if actual_dependencies != expected_dependencies:
            errors.append(
                "required-ci-gate must preserve the exact ordered "
                f"{len(REQUIRED_CI_JOBS)}-job dependency inventory"
            )
        try:
            verifier = named_step(gate, "Verify every required job succeeded")
        except AssertionError as exc:
            errors.append(str(exc))
        else:
            if re.search(r"(?m)^\s+['\"]?(?:if|continue-on-error)['\"]?:", verifier):
                errors.append("required-ci-gate verifier has a conditional/error bypass")
            required_environment = (
                "NEEDS_JSON: ${{ toJSON(needs) }}",
                f"EXPECTED_REQUIRED_JOBS_JSON: '{REQUIRED_CI_JOBS_JSON}'",
                "DEFERRED_REQUIRED_FAILURES_JSON: '{}'",
            )
            if verifier.count("env:") != 1 or any(
                verifier.count(fragment) != 1 for fragment in required_environment
            ):
                errors.append("required-ci-gate verifier must consume the exact needs and deferred-failure policy once")
            if not exact_field(
                verifier,
                "run",
                "python3 .github/scripts/verify-required-jobs.py --json-out required-ci-gate.json",
                indent=8,
            ):
                errors.append("required-ci-gate verifier must run the exact required-job script")
        try:
            upload = named_step(gate, "Upload Required CI Gate record")
        except AssertionError as exc:
            errors.append(str(exc))
        else:
            # The record must survive a red gate: the upload runs under
            # always() after the verifier exits non-zero, targets the exact
            # SHA, and fails loudly if the verifier produced no record.
            required_upload_fields = (
                ("if", "always()"),
                ("name", "required-ci-gate-${{ github.sha }}-${{ github.run_attempt }}"),
                ("path", "required-ci-gate.json"),
                ("if-no-files-found", "error"),
            )
            for field, value in required_upload_fields:
                indent = 8 if field == "if" else 10
                if not exact_field(upload, field, value, indent=indent):
                    errors.append(f"required-ci-gate record upload must set exact {field}: {value}")
            if "continue-on-error" in upload:
                errors.append("required-ci-gate record upload must not suppress its own failure")
            verifier_position = gate.find("- name: Verify every required job succeeded")
            if verifier_position < 0 or gate.index("- name: Upload Required CI Gate record") < verifier_position:
                errors.append("required-ci-gate record upload must run after the verifier")
    return errors


def required_job_bypass_errors(document: dict) -> list[str]:
    """Reject step-level error bypasses and unreviewed suppressions in required jobs.

    A step-level ``continue-on-error`` lets its job report ``success`` to the
    Required CI Gate after the step failed, and an unreviewed ``|| true`` turns
    a failed scan into a reassuring value. Both are invisible to the gate's
    ``needs.*.result`` check, so they must be rejected structurally.
    """

    errors: list[str] = []
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        return ["workflow has no jobs mapping"]
    seen: set[tuple[str, str, str]] = set()
    for job_id in REQUIRED_CI_JOBS:
        job = jobs.get(job_id)
        if not isinstance(job, dict):
            errors.append(f"required job {job_id} is missing")
            continue
        steps = job.get("steps")
        if not isinstance(steps, list) or not steps:
            errors.append(f"required job {job_id} has no steps")
            continue
        for index, step in enumerate(steps):
            if not isinstance(step, dict):
                errors.append(f"{job_id} step #{index} is not a mapping")
                continue
            name = str(step.get("name") or step.get("uses") or f"#{index}")
            if "continue-on-error" in step:
                errors.append(f"{job_id} step {name!r} declares continue-on-error")
            run = step.get("run")
            if not isinstance(run, str):
                continue
            for line in run.splitlines():
                stripped = line.strip()
                if stripped.startswith("#") or not FAILURE_SUPPRESSION.search(stripped):
                    continue
                key = (job_id, name, stripped)
                seen.add(key)
                if key not in REVIEWED_REQUIRED_JOB_SUPPRESSIONS:
                    errors.append(f"{job_id} step {name!r} suppresses a failure without review: {stripped}")
    for stale in sorted(REVIEWED_REQUIRED_JOB_SUPPRESSIONS - seen):
        errors.append(f"reviewed suppression no longer exists; remove it from the allowlist: {stale}")
    return errors


def shell_is_errexit(shell: object) -> bool:
    """True when a GitHub Actions ``shell:`` value stops on the first failing command.

    The built-in ``bash`` and ``sh`` keywords expand to ``-eo pipefail`` / ``-e``;
    a custom ``{0}`` template must carry an explicit short-option cluster with ``e``.
    Any other built-in (pwsh, python, cmd) is not a bash errexit shell.
    """

    if not isinstance(shell, str):
        return False
    words = shell.split()
    if words in (["bash"], ["sh"]):
        return True
    if "{0}" not in words or words[0].rsplit("/", 1)[-1] not in ("bash", "sh"):
        return False
    return any(re.fullmatch(r"-[a-zA-Z]*e[a-zA-Z]*", word) for word in words[1:])


def validate_all_ci_coverage_errors(document: dict, validate_all: str) -> list[str]:
    """Every validate-all check is invoked fail-closed by a required job, or is listed advisory."""

    errors: list[str] = []
    scripts = re.findall(r'(?m)^\s*run_check\s+"[^"]*"\s+"([^"]+)"', validate_all)
    if not scripts:
        return ["validate-all.sh declares no run_check invocations"]
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        return ["workflow has no jobs mapping"]
    for script in scripts:
        if script in VALIDATE_ALL_ADVISORY_CHECKS:
            continue
        invocation = VALIDATE_ALL_REQUIRED_INVOCATIONS.get(script)
        if invocation is None:
            errors.append(f"{script} is neither invoked by a required job nor listed advisory")
            continue
        job_id, command = invocation
        job = jobs.get(job_id)
        if job_id not in REQUIRED_CI_JOBS or not isinstance(job, dict):
            errors.append(f"{script} maps to {job_id}, which is not a required job")
            continue
        if "continue-on-error" in job:
            errors.append(f"{script}: required job {job_id} declares continue-on-error")
        invoking = [
            step
            for step in job.get("steps") or []
            if isinstance(step, dict)
            and isinstance(step.get("run"), str)
            and any(line.strip() == command for line in step["run"].splitlines())
        ]
        if len(invoking) != 1:
            errors.append(f"{script}: {job_id} has {len(invoking)} steps running exactly {command!r}")
            continue
        step = invoking[0]
        for field in ("if", "continue-on-error"):
            if field in step:
                errors.append(f"{script}: {job_id} step {step.get('name')!r} declares {field}")
        if re.search(r"(?m)^\s*set\s+\+e\b", step["run"]):
            errors.append(f"{script}: {job_id} step {step.get('name')!r} disables errexit")
        # The effective shell must abort on the first failing command, otherwise a
        # multi-line run can mask the check's exit status behind a later line.
        shell_sources = (
            (f"step {step.get('name')!r}", step.get("shell")),
            ("job defaults.run.shell", ((job.get("defaults") or {}).get("run") or {}).get("shell")),
            ("workflow defaults.run.shell", ((document.get("defaults") or {}).get("run") or {}).get("shell")),
        )
        for origin, shell in shell_sources:
            if shell is not None and not shell_is_errexit(shell):
                errors.append(f"{script}: {job_id} {origin} uses shell {shell!r} without errexit")
    for stale in sorted((set(VALIDATE_ALL_REQUIRED_INVOCATIONS) | set(VALIDATE_ALL_ADVISORY_CHECKS)) - set(scripts)):
        errors.append(f"{stale} is no longer run by validate-all.sh; remove its CI coverage entry")
    for overlap in sorted(set(VALIDATE_ALL_REQUIRED_INVOCATIONS) & set(VALIDATE_ALL_ADVISORY_CHECKS)):
        errors.append(f"{overlap} is listed as both required and advisory")
    return errors


def experimental_mingw_lane_errors(document: dict) -> list[str]:
    """Keep the MinGW/Wine lane manual, advisory, labeled experimental and out of the gate."""

    errors: list[str] = []
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        return ["workflow has no jobs mapping"]
    lane = jobs.get(MINGW_WINE_JOB)
    if not isinstance(lane, dict):
        return [f"{MINGW_WINE_JOB} job is missing"]
    if lane.get("if") != "github.event_name == 'workflow_dispatch'":
        errors.append(f"{MINGW_WINE_JOB} is not workflow_dispatch-only")
    if lane.get("continue-on-error") is not True:
        errors.append(f"{MINGW_WINE_JOB} does not declare job-level continue-on-error: true")
    if "experimental" not in str(lane.get("name") or ""):
        errors.append(f"{MINGW_WINE_JOB} display name does not say experimental")
    gate = jobs.get("required-ci-gate")
    if not isinstance(gate, dict):
        errors.append("required-ci-gate job is missing")
        return errors
    if MINGW_WINE_JOB in (gate.get("needs") or []):
        errors.append(f"{MINGW_WINE_JOB} is a required-ci-gate dependency")
    for step in gate.get("steps") or []:
        env = step.get("env") if isinstance(step, dict) else None
        inventory = env.get("EXPECTED_REQUIRED_JOBS_JSON") if isinstance(env, dict) else None
        if isinstance(inventory, str) and MINGW_WINE_JOB in json.loads(inventory):
            errors.append(f"{MINGW_WINE_JOB} is in EXPECTED_REQUIRED_JOBS_JSON")
    return errors


def format_filter_suffixes(script: str) -> set[str]:
    """Return the file suffixes routed to clang-format by check-format-changed.sh's case arm."""

    arms = re.findall(r"(?m)^\s+((?:\*\.[A-Za-z0-9]+\|?)+)\)\s*$", script)
    if len(arms) != 1:
        raise AssertionError(f"check-format must have exactly one source-suffix case arm, found {arms}")
    return {"." + part.split(".", 1)[1] for part in arms[0].split("|") if part}


def check_format_gate_errors(document: dict, script: str) -> list[str]:
    """check-format must run the tested script fail-closed, and validate-ci-tools must test it.

    The script is the only place the format gate's logic lives, so the job step
    must invoke exactly that script with no step condition, no continue-on-error,
    and an errexit shell, and the script itself must start under errexit.
    """

    errors: list[str] = []
    jobs = document.get("jobs")
    if not isinstance(jobs, dict):
        return ["workflow has no jobs mapping"]
    for job_id, step_name, command in (
        ("check-format", "Check formatting", CHECK_FORMAT_COMMAND),
        ("validate-ci-tools", "Test check-format controlled failures", CHECK_FORMAT_TEST_COMMAND),
    ):
        job = jobs.get(job_id)
        if not isinstance(job, dict):
            errors.append(f"{job_id} job is missing")
            continue
        for field in ("if", "continue-on-error"):
            if field in job:
                errors.append(f"{job_id} declares job-level {field}")
        steps = [step for step in job.get("steps") or [] if isinstance(step, dict) and step.get("name") == step_name]
        if len(steps) != 1:
            errors.append(f"{job_id} has {len(steps)} {step_name!r} steps")
            continue
        step = steps[0]
        if step.get("run") != command:
            errors.append(f"{job_id}/{step_name} must run exactly {command!r}, found {str(step.get('run'))[:80]!r}")
        for field in ("if", "continue-on-error"):
            if field in step:
                errors.append(f"{job_id}/{step_name} declares {field}")
        shell_sources = (
            ("step", step.get("shell")),
            ("job defaults.run.shell", ((job.get("defaults") or {}).get("run") or {}).get("shell")),
            ("workflow defaults.run.shell", ((document.get("defaults") or {}).get("run") or {}).get("shell")),
        )
        for origin, shell in shell_sources:
            if shell is not None and not shell_is_errexit(shell):
                errors.append(f"{job_id}/{step_name} {origin} uses shell {shell!r} without errexit")
    format_step = next(
        (
            step
            for step in (jobs.get("check-format") or {}).get("steps") or []
            if isinstance(step, dict) and step.get("name") == "Check formatting"
        ),
        {},
    )
    if "FORMAT_BASE_SHA" not in (format_step.get("env") or {}):
        errors.append("check-format/Check formatting no longer passes FORMAT_BASE_SHA to the script")
    code = [line.strip() for line in script.splitlines() if line.strip() and not line.strip().startswith("#")]
    if not code or code[0] != "set -euo pipefail":
        errors.append("check-format-changed.sh must start with 'set -euo pipefail'")
    if len(re.findall(r"(?m)^\s*exit\s+0\b", script)) != 1:
        errors.append("check-format-changed.sh may exit 0 only for an empty source selection")
    if re.search(r"\|\|\s*(?:exit\s+0|:)", script):
        errors.append("check-format-changed.sh suppresses a failure")
    return errors


class WorkflowFailurePropagationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = BUILD_WORKFLOW.read_text(encoding="utf-8")
        cls.release = RELEASE_WORKFLOW.read_text(encoding="utf-8")
        cls.loc_counter = LOC_COUNTER_WORKFLOW.read_text(encoding="utf-8")
        cls.site_data_publish = SITE_DATA_PUBLISH_WORKFLOW.read_text(encoding="utf-8")
        cls.readme = README.read_text(encoding="utf-8")
        cls.cmake = CMAKE_ROOT.read_text(encoding="utf-8")
        cls.build_imgui = BUILD_IMGUI_CMAKE.read_text(encoding="utf-8")
        cls.template_verifier = TEMPLATE_VERIFIER.read_text(encoding="utf-8")
        cls.template_runtime = TEMPLATE_RUNTIME_HEADER.read_text(encoding="utf-8")
        cls.fps_template = FPS_TEMPLATE_HEADER.read_text(encoding="utf-8")
        cls.tests_cmake = TESTS_CMAKE.read_text(encoding="utf-8")
        cls.test_telemetry_spool = TEST_TELEMETRY_SPOOL.read_text(encoding="utf-8")

    def test_required_workflow_semantics_are_fail_closed(self) -> None:
        self.assertEqual(required_workflow_errors(self.build), [])

    def test_todo_count_threshold_failure_is_fail_closed(self) -> None:
        job = yaml_section(self.build, "todo-count", indent=2)
        step = named_step(job, "Count TODO/FIXME/HACK comments")
        threshold_start = step.index('if [ "$count" -gt 20 ]; then')
        threshold_end = step.index("\n          fi", threshold_start)
        threshold = step[threshold_start:threshold_end]

        self.assertIn(
            'echo "::error::TODO/FIXME count ($count) exceeds threshold of 20"',
            threshold,
        )
        self.assertRegex(threshold, r"(?m)^\s+exit 1$")

    def _run_todo_count(self, *, roots: tuple[str, ...], markers: int, failing_grep: bool = False) -> tuple[int, str]:
        document = parse_workflow_yaml(self.build)
        steps = document["jobs"]["todo-count"]["steps"]
        matches = [step for step in steps if step.get("name") == "Count TODO/FIXME/HACK comments"]
        self.assertEqual(len(matches), 1)
        script = matches[0]["run"]
        with tempfile.TemporaryDirectory() as temp:
            workspace = Path(temp) / "workspace"
            for root in roots:
                (workspace / root).mkdir(parents=True)
            if roots:
                source = workspace / roots[0] / "Markers.cpp"
                source.write_text("".join(f"// TODO marker {n}\n" for n in range(markers)), encoding="utf-8")
            script_path = Path(temp) / "todo-count.sh"
            script_path.write_text(script, encoding="utf-8", newline="\n")
            output = Path(temp) / "github-output"
            output.write_text("", encoding="utf-8")
            env = dict(os.environ, GITHUB_OUTPUT=output.as_posix())
            # A shell function shadows grep on every host; a PATH shim does
            # not, because Git for Windows' bash launcher re-prepends usr/bin.
            prelude = (
                'grep() { echo "grep: simulated read error" >&2; return 2; }; '
                if failing_grep else ""
            )
            result = subprocess.run(
                [bash_executable(), "-c", prelude + '. "$0"', script_path.as_posix()],
                cwd=workspace,
                env=env,
                capture_output=True,
                text=True,
                timeout=60,
            )
            return result.returncode, output.read_text(encoding="utf-8") + result.stdout + result.stderr

    def test_todo_count_scan_failure_cannot_report_zero_markers(self) -> None:
        roots = ("SparkEngine/Source", "SparkEditor/Source", "GameModules")

        status, log = self._run_todo_count(roots=roots, markers=0)
        self.assertEqual(status, 0, log)
        self.assertIn("count=0", log)

        status, log = self._run_todo_count(roots=roots, markers=3)
        self.assertEqual(status, 0, log)
        self.assertIn("count=3", log)

        status, log = self._run_todo_count(roots=roots, markers=21)
        self.assertNotEqual(status, 0, log)

        # A missing source root once made grep exit 2, which `|| true`
        # converted into a clean zero count.
        status, log = self._run_todo_count(roots=roots[:2], markers=0)
        self.assertNotEqual(status, 0, log)
        self.assertNotIn("count=0", log)

        status, log = self._run_todo_count(roots=roots, markers=0, failing_grep=True)
        self.assertNotEqual(status, 0, log)
        self.assertNotIn("count=0", log)

    def test_required_jobs_have_no_step_bypass_or_unreviewed_suppression(self) -> None:
        self.assertEqual(required_job_bypass_errors(parse_workflow_yaml(self.build)), [])

    def test_required_job_bypass_detector_rejects_hostile_mutations(self) -> None:
        import copy

        baseline = parse_workflow_yaml(self.build)

        def run_tests_step(document: dict) -> dict:
            steps = document["jobs"]["build-linux-gcc"]["steps"]
            return next(step for step in steps if step.get("name") == "Run Tests")

        advisory = copy.deepcopy(baseline)
        run_tests_step(advisory)["continue-on-error"] = True
        self.assertIn(
            "build-linux-gcc step 'Run Tests' declares continue-on-error",
            required_job_bypass_errors(advisory),
        )

        # Even an explicit false is rejected: the key is a toggle away from a bypass.
        explicit_false = copy.deepcopy(baseline)
        run_tests_step(explicit_false)["continue-on-error"] = False
        self.assertTrue(required_job_bypass_errors(explicit_false))

        for suffix in (" || true", " || :", ' || echo "ignored"', " || exit 0"):
            with self.subTest(suffix=suffix):
                suppressed = copy.deepcopy(baseline)
                step = run_tests_step(suppressed)
                step["run"] = step["run"].replace(
                    "--output-junit ctest-junit.xml 2>&1 | tee test-results.log",
                    "--output-junit ctest-junit.xml 2>&1 | tee test-results.log" + suffix,
                )
                self.assertNotEqual(step["run"], run_tests_step(baseline)["run"])
                self.assertTrue(
                    any("suppresses a failure without review" in error for error in required_job_bypass_errors(suppressed))
                )

        # Reverting the todo-count scan to the old swallowed-grep form is rejected.
        legacy = copy.deepcopy(baseline)
        todo = next(step for step in legacy["jobs"]["todo-count"]["steps"] if step.get("name") == "Count TODO/FIXME/HACK comments")
        todo["run"] += "\ngrep -rn 'TODO' SparkEngine/Source > todo-list.log || true\n"
        self.assertTrue(
            any(error.startswith("todo-count step") for error in required_job_bypass_errors(legacy))
        )

        # A reviewed line cannot be silently moved into an evidence step.
        moved = copy.deepcopy(baseline)
        step = run_tests_step(moved)
        step["run"] += "\nfind build -name '*.pch' -delete 2>/dev/null || true\n"
        self.assertTrue(
            any("'Run Tests' suppresses a failure without review" in error for error in required_job_bypass_errors(moved))
        )

    def test_check_format_routes_every_tracked_cxx_suffix(self) -> None:
        script = CHECK_FORMAT_SCRIPT.read_text(encoding="utf-8")
        roots_match = re.search(r"(?m)^FORMAT_ROOTS=\((?P<roots>[^)]*)\)\s*$", script)
        self.assertIsNotNone(roots_match)
        assert roots_match is not None
        self.assertEqual(tuple(roots_match.group("roots").split()), FORMAT_ROOTS)

        routed = format_filter_suffixes(script)
        present: dict[str, str] = {}
        for root in FORMAT_ROOTS:
            for directory, _dirs, files in os.walk(REPO_ROOT / root):
                if "Metal" in Path(directory).relative_to(REPO_ROOT).parts:
                    continue
                for file_name in files:
                    suffix = Path(file_name).suffix.lower()
                    if suffix in CXX_SOURCE_SUFFIXES:
                        present.setdefault(suffix, str(Path(directory, file_name).relative_to(REPO_ROOT)))
        self.assertIn(".cpp", present)
        missing = {suffix: example for suffix, example in present.items() if suffix not in routed}
        self.assertEqual(missing, {}, "check-format silently skips tracked C/C++ sources")

    def test_check_format_runs_the_tested_script_fail_closed(self) -> None:
        script = CHECK_FORMAT_SCRIPT.read_text(encoding="utf-8")
        self.assertEqual(check_format_gate_errors(parse_workflow_yaml(self.build), script), [])

    def test_check_format_gate_contract_rejects_each_bypass(self) -> None:
        baseline = parse_workflow_yaml(self.build)
        script = CHECK_FORMAT_SCRIPT.read_text(encoding="utf-8")

        def step(jobs: dict, job_id: str, name: str) -> dict:
            return next(item for item in jobs[job_id]["steps"] if item.get("name") == name)

        def mutate(change, mutated_script: str = script) -> list[str]:
            document = copy.deepcopy(baseline)
            change(document["jobs"])
            return check_format_gate_errors(document, mutated_script)

        format_step = lambda jobs: step(jobs, "check-format", "Check formatting")  # noqa: E731
        test_step = lambda jobs: step(jobs, "validate-ci-tools", "Test check-format controlled failures")  # noqa: E731
        cases = (
            (lambda jobs: format_step(jobs).update({"continue-on-error": True}), "declares continue-on-error"),
            (lambda jobs: format_step(jobs).update({"if": "success()"}), "declares if"),
            (lambda jobs: format_step(jobs).update({"shell": "bash {0}"}), "without errexit"),
            (lambda jobs: format_step(jobs).update({"run": CHECK_FORMAT_COMMAND + " || true"}), "must run exactly"),
            (lambda jobs: format_step(jobs).update({"run": "echo skipped"}), "must run exactly"),
            (lambda jobs: format_step(jobs).pop("env"), "FORMAT_BASE_SHA"),
            (lambda jobs: jobs["check-format"].update({"continue-on-error": True}), "job-level continue-on-error"),
            (lambda jobs: test_step(jobs).update({"continue-on-error": True}), "declares continue-on-error"),
            (lambda jobs: jobs["validate-ci-tools"]["steps"].remove(test_step(jobs)), "has 0"),
        )
        for change, expected in cases:
            with self.subTest(expected=expected):
                errors = mutate(change)
                self.assertTrue(any(expected in error for error in errors), errors)

        no_errexit = script.replace("set -euo pipefail\n", "set -uo pipefail\n", 1)
        self.assertIn("must start with 'set -euo pipefail'", " ".join(mutate(lambda jobs: None, no_errexit)))
        swallowed = script.replace('exit "$FORMAT_STATUS"', 'exit "$FORMAT_STATUS" || :')
        self.assertIn("suppresses a failure", " ".join(mutate(lambda jobs: None, swallowed)))
        early_pass = script.replace("set -euo pipefail\n", "set -euo pipefail\nexit 0\n", 1)
        self.assertIn("exit 0 only", " ".join(mutate(lambda jobs: None, early_pass)))

    def test_clang_tidy_inventory_covers_all_shipped_source_roots(self) -> None:
        block = self.build[self.build.index("\n  clang-tidy:\n"):]
        block = block[:block.index("\n  # ===========================================================================", 1)]
        self.assertNotIn("head -z", block)
        self.assertIn("file_count=$(tr -cd '\\0' < clang-tidy-files.list | wc -c)", block)
        self.assertIn('if [ "$file_count" -eq 0 ]; then', block)
        self.assertIn('if [ ! -d "$root" ]; then', block)
        for root in CLANG_TIDY_SOURCE_ROOTS:
            self.assertIn(f"            {root}\n", block)

    def test_required_ci_verifier_covers_every_declared_job(self) -> None:
        document = parse_workflow_yaml(self.build)
        gate = document["jobs"]["required-ci-gate"]
        declared_jobs = gate["needs"]
        verifier = next(
            step
            for step in gate["steps"]
            if step.get("name") == "Verify every required job succeeded"
        )
        enforced_jobs = json.loads(verifier["env"]["EXPECTED_REQUIRED_JOBS_JSON"])
        self.assertEqual(enforced_jobs, declared_jobs)

    def test_license_compliance_job_is_required_and_fail_closed(self) -> None:
        document = parse_workflow_yaml(self.build)
        job = document["jobs"].get("license-compliance")
        self.assertIsInstance(job, dict)
        assert isinstance(job, dict)
        report = document["jobs"]["report-ci-errors"]
        self.assertIn("license-compliance", report["needs"])
        self.assertEqual(job.get("runs-on"), "ubuntu-24.04")
        self.assertEqual(job.get("permissions"), {"contents": "read"})
        self.assertNotIn("if", job)
        self.assertNotIn("continue-on-error", job)

        steps = job.get("steps")
        self.assertIsInstance(steps, list)
        assert isinstance(steps, list)
        legal_steps = [
            step
            for step in steps
            if isinstance(step, dict) and step.get("name") == "Run legal contract validator"
        ]
        self.assertEqual(len(legal_steps), 1)
        legal_step = legal_steps[0]
        self.assertNotIn("if", legal_step)
        self.assertNotIn("continue-on-error", legal_step)
        self.assertEqual(
            str(legal_step.get("run", "")).strip(),
            "set -euo pipefail\ntimeout 3m python3 tools/site-data/validate.py --legal",
        )
        self.assertNotIn("|| true", str(legal_step.get("run", "")))

    def test_release_workflow_is_yaml_parseable(self) -> None:
        document = yaml.safe_load(self.release)
        self.assertIsInstance(document, dict)
        self.assertIn("jobs", document)

    def test_git_bash_is_available_to_release_workflow_fixtures(self) -> None:
        self.assertTrue(Path(bash_executable()).is_file())

    @unittest.skipUnless(os.name == "nt", "Windows release fixture shell selection")
    def test_windows_release_fixtures_prefer_git_bash_over_wsl_launcher(self) -> None:
        """Fixture cwd/env semantics must match the Windows release workflow shell."""
        expected = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe"
        self.assertTrue(expected.is_file(), f"Git Bash is unavailable at {expected}")
        self.assertEqual(Path(bash_executable()).resolve(), expected.resolve())

    @unittest.skipUnless(os.name == "nt", "Windows release fixture shell selection")
    def test_windows_release_fixtures_reject_wsl_launcher_when_git_bash_is_absent(self) -> None:
        """WSL's launcher cannot silently replace the workflow's Git Bash semantics."""
        wsl_launcher = Path(os.environ["LOCALAPPDATA"]) / "Microsoft" / "WindowsApps" / "bash.exe"
        self.assertTrue(wsl_launcher.is_file(), f"WSL launcher fixture is unavailable at {wsl_launcher}")
        with mock.patch.object(Path, "is_file", return_value=False), \
                mock.patch.object(shutil, "which", return_value=str(wsl_launcher)):
            with self.assertRaisesRegex(FileNotFoundError, "Git Bash"):
                bash_executable()

    def test_ci_runs_release_acceptance_gate_regressions(self) -> None:
        validation = yaml_section(self.build, "validate-ci-tools", indent=2)
        acceptance_tests = named_step(validation, "Test release acceptance publication gate")
        self.assertTrue(
            exact_field(
                acceptance_tests,
                "run",
                "python3 .github/scripts/test-release-acceptance-gate.py",
                indent=6,
            )
        )

    def test_static_build_matrix_baseline_does_not_duplicate_producer_enforcement(self) -> None:
        static_validation = yaml_section(self.build, "validate-ci-tools", indent=2)
        structural_producer = yaml_section(self.build, "build-windows-shipping", indent=2)
        enforcement_name = "- name: Record build-matrix evidence"

        self.assertNotIn(enforcement_name, static_validation)
        self.assertEqual(structural_producer.count(enforcement_name), 1)
        self.assertIn("Compare reviewed build-matrix findings", static_validation)
        self.assertIn("Upload deterministic build-matrix evidence", static_validation)

    def test_release_artifact_builders_use_explicit_runner_images(self) -> None:
        for workflow in (self.build, self.release):
            self.assertNotIn("windows-latest", workflow)
            self.assertNotIn("macos-latest", workflow)
        self.assertIn("runs-on: windows-2025-vs2026", self.build)
        self.assertIn("- os: windows-2022", self.build)
        self.assertIn("- os: macos-15", self.build)
        self.assertIn("- os: windows-2022", self.release)
        self.assertIn("- os: macos-15", self.release)

    def test_macos_install_rpath_is_initialized_before_engine_targets(self) -> None:
        """Installed macOS binaries must inherit the staged shared-library RPATH."""
        rpath = 'set(CMAKE_INSTALL_RPATH "@executable_path;@executable_path/../lib")'
        self.assertEqual(self.cmake.count(rpath), 1)
        rpath_position = self.cmake.index(rpath)
        first_engine_library = self.cmake.index("add_library(SparkEngineLib STATIC")
        first_engine_executable = self.cmake.index("add_executable(SparkEngine")
        self.assertLess(rpath_position, first_engine_library)
        self.assertLess(rpath_position, first_engine_executable)
        self.assertIn("set(CMAKE_MACOSX_RPATH ON)", self.cmake)
        self.assertIn("set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)", self.cmake)

    def test_linux_install_rpath_is_initialized_before_engine_targets(self) -> None:
        """Installed Linux binaries must resolve shared libraries from package lib/."""
        rpath = 'set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")'
        self.assertEqual(self.cmake.count(rpath), 1)
        rpath_position = self.cmake.index(rpath)
        first_engine_library = self.cmake.index("add_library(SparkEngineLib STATIC")
        first_engine_executable = self.cmake.index("add_executable(SparkEngine")
        self.assertLess(rpath_position, first_engine_library)
        self.assertLess(rpath_position, first_engine_executable)

    def test_installed_template_smoke_captures_sandboxed_engine_log(self) -> None:
        """GUI-subsystem Windows hosts must expose FileSink evidence to the verifier."""
        verifier = self.template_verifier
        self.assertIn('COMMAND "${CMAKE_COMMAND}" -E env', verifier)
        self.assertIn('"LOCALAPPDATA=${live_smoke_user_data}"', verifier)
        self.assertIn('"XDG_DATA_HOME=${live_smoke_user_data}"', verifier)
        self.assertIn('file(GLOB live_smoke_logs "${live_smoke_user_data}/SparkEngine/Logs/', verifier)
        self.assertIn('file(READ "${live_smoke_log}" live_smoke_log_text)', verifier)
        self.assertIn('string(APPEND live_smoke_log_output', verifier)
        self.assertIn(
            'set(live_smoke_output "${live_smoke_stdout}${live_smoke_stderr}${live_smoke_log_output}")',
            verifier,
        )

    def test_template_bridge_emits_opt_in_smoke_evidence_marker(self) -> None:
        """Module-local logging must not be the only scene-ownership evidence."""
        self.assertIn("SPARK_TEMPLATE_LIVE_SMOKE_EVIDENCE", self.template_runtime)
        self.assertIn('".spark-template-live-smoke.log"', self.template_runtime)
        self.assertIn('"SPARK_TEMPLATE_LIVE_SMOKE_EVIDENCE=1"', self.template_verifier)
        self.assertIn('file(READ "${live_smoke_evidence}" live_smoke_evidence_output)', self.template_verifier)
        self.assertIn('string(APPEND live_smoke_output "${live_smoke_evidence_output}")', self.template_verifier)
        self.assertIn('"Game/TemplateRuntime.h"', self.fps_template)
        self.assertIn('Spark::Templates::EmitLiveSmokeEvidence("FPSStarter"', self.fps_template)

    def test_posix_sdl_install_uses_regular_files_for_portable_archives(self) -> None:
        """Portable POSIX packages must dereference SDL2 links without renaming its ABI."""
        start = self.cmake.index("if(TARGET SDL2 AND EXISTS")
        end = self.cmake.index("set(SPARK_PACKAGE_HAS_CURL", start)
        sdl_install = self.cmake[start:end]
        self.assertIn("install(CODE", sdl_install)
        self.assertIn('file(GLOB _spark_sdl2_entries "${_spark_sdl2_libdir}/libSDL2*")', sdl_install)
        self.assertIn('if(IS_SYMLINK "${_spark_sdl2_entry}")', sdl_install)
        self.assertIn('file(REAL_PATH "${_spark_sdl2_entry}" _spark_sdl2_resolved)', sdl_install)
        self.assertIn('file(REMOVE "${_spark_sdl2_entry}")', sdl_install)
        self.assertIn('file(COPY_FILE "${_spark_sdl2_resolved}" "${_spark_sdl2_entry}")', sdl_install)

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
        selected_tests = re.findall(
            r"^TEST\(Telemetry_SpoolRecovery(?:_[A-Za-z0-9_]+)?\)\s*$",
            self.test_telemetry_spool,
            flags=re.MULTILINE,
        )
        self.assertEqual(len(selected_tests), TELEMETRY_EXPECTED_COUNT)

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
            "        run: python3 .github/scripts/verify-required-jobs.py --json-out required-ci-gate.json",
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
            "  push:\n    branches: [ main, develop, Working, 'release/**' ]",
            "  push:\n    branches: [ main, develop, Working, 'release/**' ]\n    paths-ignore: ['**']",
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

    def test_required_gate_record_upload_survives_gate_failure(self) -> None:
        self.assertEqual(required_workflow_errors(self.build), [])
        gate = yaml_section(self.build, "required-ci-gate", indent=2)
        upload = named_step(gate, "Upload Required CI Gate record")
        self.assertIn("actions/upload-artifact@", upload)
        record_marker = "\n      - name: Upload Required CI Gate record\n        if: always()\n"
        name_marker = "name: required-ci-gate-${{ github.sha }}-${{ github.run_attempt }}"
        cases = {
            "upload skipped on red gate": (
                record_marker,
                record_marker.replace("if: always()", "if: success()"),
                "exact if: always()",
            ),
            "upload silently optional": (
                "          path: required-ci-gate.json\n"
                "          retention-days: ${{ env.ARTIFACT_RETENTION_DAYS }}\n"
                "          if-no-files-found: error\n",
                "          path: required-ci-gate.json\n"
                "          retention-days: ${{ env.ARTIFACT_RETENTION_DAYS }}\n"
                "          if-no-files-found: warn\n",
                "exact if-no-files-found: error",
            ),
            "upload not bound to exact sha": (
                name_marker,
                "name: required-ci-gate-latest",
                "exact name:",
            ),
            "upload suppresses failure": (
                record_marker,
                record_marker + "        continue-on-error: true\n",
                "must not suppress its own failure",
            ),
            "verifier writes no record": (
                "        run: python3 .github/scripts/verify-required-jobs.py --json-out required-ci-gate.json\n",
                "        run: python3 .github/scripts/verify-required-jobs.py\n",
                "must run the exact required-job script",
            ),
            "upload removed": (
                "      - name: Upload Required CI Gate record\n",
                "      - name: Upload gate notes\n",
                "Upload Required CI Gate record",
            ),
        }
        for label, (old, new, message) in cases.items():
            with self.subTest(mutation=label):
                self.assertEqual(self.build.count(old), 1, label)
                mutated = self.build.replace(old, new, 1)
                errors = required_workflow_errors(mutated)
                self.assertTrue(any(message in error for error in errors), errors)

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

    def test_final_publication_paths_use_acceptance_gate(self) -> None:
        self.assertEqual(release_acceptance_gate_errors(self.release), [])

    def test_release_acceptance_recovery_waits_for_patch_marker(self) -> None:
        self.assertEqual(release_acceptance_recovery_errors(self.release), [])

    def test_release_acceptance_gate_rejects_bare_patch_mutation(self) -> None:
        acceptance_command = 'python3 -I "$GITHUB_WORKSPACE/.github/scripts/release-acceptance-gate.py"'
        for step_name in (
            "Publish complete stable versioned release",
            "Publish complete nightly rolling release",
        ):
            step = named_step(self.release, step_name)
            mutated = self.release.replace(
                step,
                step.replace(acceptance_command, "gh api --method PATCH"),
                1,
            )
            with self.subTest(step_name=step_name):
                errors = release_acceptance_gate_errors(mutated)
                self.assertIn(
                    f"{step_name} must not publish through a bare gh API PATCH",
                    errors,
                )

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
            "Verify stable-v1 candidate is qualified for versioned publication",
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
                readiness,
                readiness.replace(
                    "      if: needs.prepare.outputs.is_versioned == 'true'",
                    "      if: needs.prepare.outputs.is_versioned == 'true' || github.event_name == 'workflow_dispatch'",
                    1,
                ),
                1,
            ),
            # The mutants are derived from the step itself rather than from a
            # copy of its command text: a hard-coded copy stops matching the
            # moment the command changes, and then "mutation not detected"
            # becomes "mutation never applied" -- a fixture that cannot fail.
            "suppressed validator": self.release.replace(
                readiness,
                suppress_run_command(readiness),
                1,
            ),
            "predecessor validator bypass": self.release.replace(
                readiness,
                readiness.replace(
                    "--require-predecessor-candidate",
                    "--require-candidate-ready",
                    1,
                ),
                1,
            ),
            "stable-v1 validator bypass": self.release.replace(
                readiness,
                readiness.replace(
                    "--require-candidate-ready",
                    "--require-predecessor-candidate",
                    1,
                ),
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
                readiness,
                inject_before_run(readiness, "      continue-on-error: true"),
                1,
            ),
            "quoted continue on error": self.release.replace(
                readiness,
                inject_before_run(readiness, "      'continue-on-error': true"),
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

    def test_installer_builds_every_registered_contract_test(self) -> None:
        build_step = named_step(self.build, "Build SparkInstaller and registered contract tests")
        self.assertIn("SparkInstallerGitTests", build_step)
        self.assertIn("SparkInstallerInstallStateTests", build_step)
        self.assertIn("SparkInstallerTransactionTests", build_step)
        self.assertIn("SparkBuildProcessRunnerTests", build_step)
        self.assertIn("SparkBuildDownloaderTests", build_step)

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

    def test_every_validate_all_check_is_fail_closed_in_a_required_job(self) -> None:
        validate_all = VALIDATE_ALL.read_text(encoding="utf-8")
        self.assertEqual(validate_all_ci_coverage_errors(parse_workflow_yaml(self.build), validate_all), [])

    def test_validate_all_ci_coverage_rejects_removed_or_suppressed_checks(self) -> None:
        baseline = parse_workflow_yaml(self.build)
        validate_all = VALIDATE_ALL.read_text(encoding="utf-8")
        command = VALIDATE_ALL_REQUIRED_INVOCATIONS["check-wiring.sh"][1]

        def wiring_step(document):
            return next(
                step
                for step in document["jobs"]["validate-ci-tools"]["steps"]
                if isinstance(step.get("run"), str) and command in step["run"]
            )

        def mutate(change):
            document = copy.deepcopy(baseline)
            change(document)
            return validate_all_ci_coverage_errors(document, validate_all)

        cases = (
            (
                lambda document: document["jobs"]["validate-ci-tools"]["steps"].remove(wiring_step(document)),
                "has 0 steps running exactly",
            ),
            (
                lambda document: wiring_step(document).update({"run": command + " || true"}),
                "has 0 steps running exactly",
            ),
            (lambda document: wiring_step(document).update({"continue-on-error": True}), "declares continue-on-error"),
            (lambda document: wiring_step(document).update({"if": "false"}), "declares if"),
            (
                lambda document: wiring_step(document).update({"run": "set +e\n" + command}),
                "disables errexit",
            ),
            (
                lambda document: document["jobs"]["validate-ci-tools"].update({"continue-on-error": True}),
                "declares continue-on-error",
            ),
            (
                lambda document: wiring_step(document).update({"shell": "bash {0}", "run": command + "\ntrue"}),
                "without errexit",
            ),
            (lambda document: wiring_step(document).update({"shell": "pwsh"}), "without errexit"),
            (
                lambda document: document["jobs"]["validate-ci-tools"].update(
                    {"defaults": {"run": {"shell": "bash --noprofile --norc {0}"}}}
                ),
                "job defaults.run.shell uses shell",
            ),
            (
                lambda document: document.update({"defaults": {"run": {"shell": "sh {0}"}}}),
                "workflow defaults.run.shell uses shell",
            ),
        )
        for change, message in cases:
            with self.subTest(message=message):
                errors = mutate(change)
                self.assertTrue(any(message in error for error in errors), errors)

        unlisted = validate_all.replace(
            'run_check "System Wiring"',
            'run_check "Unlisted Probe"               "check-unlisted-probe.sh"\nrun_check "System Wiring"',
        )
        self.assertIn(
            "check-unlisted-probe.sh is neither invoked by a required job nor listed advisory",
            validate_all_ci_coverage_errors(baseline, unlisted),
        )
        dropped = validate_all.replace('run_check "System Wiring"                "check-wiring.sh"\n', "")
        self.assertNotEqual(dropped, validate_all)
        self.assertIn(
            "check-wiring.sh is no longer run by validate-all.sh; remove its CI coverage entry",
            validate_all_ci_coverage_errors(baseline, dropped),
        )

    def test_errexit_shell_classification(self) -> None:
        for shell in ("bash", "sh", "bash -eo pipefail {0}", "bash --noprofile --norc -e {0}", "/bin/sh -ex {0}"):
            with self.subTest(shell=shell):
                self.assertTrue(shell_is_errexit(shell))
        for shell in ("bash {0}", "bash --noprofile --norc -o pipefail {0}", "pwsh", "python", "cmd", "", None):
            with self.subTest(shell=shell):
                self.assertFalse(shell_is_errexit(shell))

    def test_mingw_wine_lane_is_manual_advisory_and_labeled_experimental(self) -> None:
        self.assertEqual(experimental_mingw_lane_errors(parse_workflow_yaml(self.build)), [])

    def test_mingw_wine_lane_contract_rejects_each_property_regression(self) -> None:
        baseline = parse_workflow_yaml(self.build)
        self.assertNotIn(MINGW_WINE_JOB, REQUIRED_CI_JOBS)

        def mutate(change):
            document = copy.deepcopy(baseline)
            change(document["jobs"])
            return experimental_mingw_lane_errors(document)

        cases = (
            (lambda jobs: jobs[MINGW_WINE_JOB].pop("if", None), "is not workflow_dispatch-only"),
            (lambda jobs: jobs[MINGW_WINE_JOB].update({"if": "always()"}), "is not workflow_dispatch-only"),
            (lambda jobs: jobs[MINGW_WINE_JOB].pop("continue-on-error", None), "continue-on-error: true"),
            (
                lambda jobs: jobs[MINGW_WINE_JOB].update({"continue-on-error": False}),
                "continue-on-error: true",
            ),
            (lambda jobs: jobs[MINGW_WINE_JOB].pop("name", None), "does not say experimental"),
            (
                lambda jobs: jobs[MINGW_WINE_JOB].update({"name": "build-linux-mingw-wine"}),
                "does not say experimental",
            ),
            (
                lambda jobs: jobs["required-ci-gate"]["needs"].append(MINGW_WINE_JOB),
                "is a required-ci-gate dependency",
            ),
            (
                lambda jobs: [
                    step["env"].update(
                        {
                            "EXPECTED_REQUIRED_JOBS_JSON": json.dumps(
                                [*REQUIRED_CI_JOBS, MINGW_WINE_JOB], separators=(",", ":")
                            )
                        }
                    )
                    for step in jobs["required-ci-gate"]["steps"]
                    if isinstance(step.get("env"), dict) and "EXPECTED_REQUIRED_JOBS_JSON" in step["env"]
                ],
                "is in EXPECTED_REQUIRED_JOBS_JSON",
            ),
        )
        for change, message in cases:
            with self.subTest(message=message):
                errors = mutate(change)
                self.assertTrue(any(message in error for error in errors), errors)

    def test_msan_is_verified_but_remains_optional(self) -> None:
        msan_block = named_step(self.build, "Run Tests under MSan")
        self.assertIn("--runtime-env MSAN_OPTIONS", msan_block)
        self.assertIn("--sanitizer msan", msan_block)

        # The hosted MSan lane cannot make the distro FreeType shared library
        # instrumented.  If the optional ImGui backend is enabled, editor font
        # tests enter that uninstrumented library and produce an incomplete
        # sanitizer run before JUnit can be written.  Keep MSan strict by
        # making the build select ImGui's instrumented-free stb path instead.
        self.assertIn("-DSPARK_IMGUI_ENABLE_FREETYPE=OFF", self.build)
        self.assertIn(
            'option(SPARK_IMGUI_ENABLE_FREETYPE "Enable optional FreeType rasterizer" ON)',
            self.build_imgui,
        )
        self.assertIn(
            "if(SPARK_IMGUI_ENABLE_FREETYPE)\n            find_package(Freetype QUIET)",
            self.build_imgui,
        )
        self.assertIn(
            "else()\n            set(Freetype_FOUND FALSE)",
            self.build_imgui,
        )

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

    def test_sanitizer_jobs_and_test_processes_have_policy_specific_timeouts(self) -> None:
        for sanitizer in ("asan", "tsan"):
            start = self.build.index(f"build-linux-{sanitizer}:")
            next_job = self.build.index("\n  build-", start + 1)
            section = self.build[start:next_job]
            self.assertIn("timeout-minutes: 90", section)
            self.assertIn("--timeout-seconds 900", section)

        msan_start = self.build.index("build-linux-msan:")
        msan_next_job = self.build.index("\n  build-", msan_start + 1)
        msan_section = self.build[msan_start:msan_next_job]
        self.assertIn("timeout-minutes: 120", msan_section)
        self.assertIn("--timeout-seconds 5400", msan_section)

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

    def test_sanitizer_exact_commit_artifacts_survive_test_failure(self) -> None:
        for job_id, step_name in (
            ("build-linux-asan", "Upload ASan exact-commit test evidence"),
            ("build-linux-tsan", "Upload TSan exact-commit test evidence"),
        ):
            with self.subTest(job_id=job_id):
                job = yaml_section(self.build, job_id, indent=2)
                upload = named_step(job, step_name)
                self.assertEqual(
                    re.search(r"(?m)^      if: (.+)$", upload).group(1),
                    "always()",
                )

    def test_working_pushes_are_not_cancelled_before_evidence_finishes(self) -> None:
        self.assertIn("|| github.sha }}", self.build)
        self.assertIn(
            "cancel-in-progress: ${{ github.event_name == 'pull_request' }}",
            self.build,
        )

    def test_workflow_dispatch_reaches_required_gate_without_event_skips(self) -> None:
        document = parse_workflow_yaml(self.build)
        triggers = document.get("on")
        self.assertIsInstance(triggers, dict)
        assert isinstance(triggers, dict)
        self.assertIn("workflow_dispatch", triggers)

        jobs = document["jobs"]
        gate = jobs.get("required-ci-gate")
        self.assertIsInstance(gate, dict)
        assert isinstance(gate, dict)
        self.assertEqual(gate.get("if"), "always()")
        self.assertEqual(gate.get("needs"), list(REQUIRED_CI_JOBS))

        for job_id in REQUIRED_CI_JOBS:
            with self.subTest(job_id=job_id):
                job = jobs.get(job_id)
                self.assertIsInstance(job, dict)
                assert isinstance(job, dict)
                if job_id == "aggregate-test-stats":
                    self.assertEqual(job.get("if"), "always()")
                else:
                    self.assertNotIn("if", job)
                self.assertNotIn("continue-on-error", job)

        concurrency = document.get("concurrency")
        self.assertIsInstance(concurrency, dict)
        assert isinstance(concurrency, dict)
        self.assertEqual(concurrency.get("cancel-in-progress"), "${{ github.event_name == 'pull_request' }}")

    def test_manual_required_failure_probe_is_explicit_and_default_off(self) -> None:
        document = parse_workflow_yaml(self.build)
        triggers = document.get("on")
        self.assertIsInstance(triggers, dict)
        assert isinstance(triggers, dict)
        dispatch = triggers.get("workflow_dispatch")
        self.assertIsInstance(dispatch, dict)
        assert isinstance(dispatch, dict)

        inputs = dispatch.get("inputs")
        self.assertIsInstance(inputs, dict)
        assert isinstance(inputs, dict)
        probe = inputs.get("simulate_required_job_failure")
        self.assertIsInstance(probe, dict)
        assert isinstance(probe, dict)
        self.assertEqual(probe.get("type"), "boolean")
        self.assertEqual(probe.get("default"), False)
        self.assertEqual(probe.get("required"), False)

        jobs = document["jobs"]
        tooling = jobs.get("validate-ci-tools")
        self.assertIsInstance(tooling, dict)
        assert isinstance(tooling, dict)
        steps = tooling.get("steps")
        self.assertIsInstance(steps, list)
        assert isinstance(steps, list)
        step = next(
            (candidate for candidate in steps if candidate.get("name") == "Controlled required-job failure probe"),
            None,
        )
        self.assertIsNotNone(step)
        assert isinstance(step, dict)
        self.assertEqual(
            step.get("if"),
            "github.event_name == 'workflow_dispatch' && inputs.simulate_required_job_failure == true",
        )
        self.assertIn("exit 1", step.get("run", ""))
        self.assertNotIn("continue-on-error", tooling)

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

    def test_generated_metrics_have_a_dedicated_publication_queue(self) -> None:
        self.assertIn("  group: sparkengine-repository-metrics-publication", self.loc_counter)
        self.assertIn("  cancel-in-progress: false", self.loc_counter)
        self.assertNotIn("  group: sparkengine-publication-global", self.loc_counter)

    def test_readme_ci_badge_tracks_fail_closed_aggregate_workflow(self) -> None:
        self.assertIn("[![Trusted exact-source CI]", self.readme)
        self.assertIn(
            "https://github.com/Krilliac/SparkEngine/actions/workflows/"
            "trusted-ci-aggregate.yml/badge.svg?branch=Working",
            self.readme,
        )
        self.assertIn(
            "https://github.com/Krilliac/SparkEngine/actions/workflows/"
            "trusted-ci-aggregate.yml?query=branch%3AWorking",
            self.readme,
        )
        self.assertNotIn("[![Current commit checks]", self.readme)
        self.assertNotIn("github/check-runs", self.readme)
        self.assertNotIn("github/checks-status", self.readme)
        self.assertNotIn("github/check-suites", self.readme)

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
        self.assertEqual(
            aggregate.count("statusContract.loadBoundedCommitStatusHistory"),
            7,
            "Every aggregate status-history read must use the bounded authenticated loader.",
        )
        self.assertEqual(
            aggregate.count(
                "listPage: ({ page, per_page }) => github.rest.repos.listCommitStatusesForRef"
            ),
            7,
            "Every bounded history loader must bind its own GitHub status endpoint.",
        )
        self.assertIn(
            "statuses.length > statusContract.COMMIT_STATUS_HISTORY_MAX_RECORDS",
            aggregate,
        )
        header = aggregate[: aggregate.index("jobs:")]
        self.assertIn('name: "Trusted Exact-Source CI"', aggregate)
        self.assertIn("push:\n    branches: [Working]", header)
        self.assertIn(
            'workflows: ["Build Matrix Verifier", "CodeQL Trusted Reporter"]',
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
        self.assertIn("Reap abandoned exact-source badge checks", aggregate)
        self.assertIn("Detect terminal-ready exact evidence", aggregate)
        self.assertIn("Recheck terminal-ready exact evidence", aggregate)
        self.assertIn("Neutralize superseded exact-source badge check", aggregate)
        self.assertIn("Finalize exact aggregate status", aggregate)
        self.assertIn("Trusted Exact-Source CI / Aggregate", aggregate)
        self.assertIn("trusted-ci-aggregate-status.js", aggregate)
        self.assertIn("hasCurrentPendingLease", aggregate)
        self.assertIn("getCombinedStatusForRef", aggregate)
        self.assertIn("listCommitStatusesForRef", aggregate)
        self.assertNotIn("status.sha", aggregate)
        terminal_ready = "steps.terminal-readiness.outputs.ready == 'true'"
        readiness = named_step(aggregate, "Detect terminal-ready exact evidence")
        self.assertIn("hasTerminalReporterSet", readiness)
        self.assertIn("isExactCompletedReporterEvent", readiness)
        self.assertIn("getCombinedStatusForRef", readiness)
        self.assertIn("listCommitStatusesForRef", readiness)
        self.assertIn("getWorkflowRun", readiness)
        self.assertIn("context.eventName === 'schedule'", readiness)
        self.assertIn("context.payload.workflow_run", readiness)
        self.assertIn("context.payload.action", readiness)
        recheck = named_step(aggregate, "Recheck terminal-ready exact evidence")
        self.assertIn("hasTerminalReporterSet", recheck)
        self.assertIn("getCombinedStatusForRef", recheck)
        self.assertIn("listCommitStatusesForRef", recheck)
        self.assertIn("isExactCompletedReporterEvent", recheck)
        self.assertIn("getWorkflowRun", recheck)
        reaper = named_step(aggregate, "Reap abandoned exact-source badge checks")
        self.assertIn(
            "if: always() && steps.trusted-attestation.outcome == 'success'",
            reaper,
        )
        self.assertIn("check.status === 'in_progress'", reaper)
        self.assertIn("isCompletedSuccess", reaper)
        self.assertIn("run.conclusion === 'success'", reaper)
        self.assertIn("getWorkflowRunAttempt", reaper)
        self.assertIn("attempt_number: runAttempt", reaper)
        self.assertIn("conclusion: 'neutral'", reaper)
        neutralizer = named_step(aggregate, "Neutralize superseded exact-source badge check")
        self.assertIn("conclusion: 'neutral'", neutralizer)
        badge_create = named_step(aggregate, "Create pending exact-source badge check")
        self.assertIn(f"if: {terminal_ready}", badge_create)
        exact_gate = named_step(aggregate, "Verify both trusted exact-source evidence chains")
        self.assertIn(f"if: {terminal_ready}", exact_gate)
        finalize_badge = named_step(aggregate, "Finalize exact-source badge check")
        self.assertIn(
            "steps.terminal-publication-readiness.outputs.ready == 'true'",
            finalize_badge,
        )
        badge_reporter_race = finalize_badge[
            finalize_badge.index("if (!liveTerminal)") : finalize_badge.index("const outcomes")
        ]
        self.assertIn("core.setFailed", badge_reporter_race)
        self.assertIn(
            "current.status === 'completed' && current.conclusion === 'neutral'",
            finalize_badge,
        )
        self.assertNotIn("if (current.status === 'completed') return", finalize_badge)
        finalize_status = named_step(aggregate, "Finalize exact aggregate status")
        self.assertIn(
            "if: always() && steps.aggregate-status.outputs.status-id != '' && "
            + "steps.terminal-publication-readiness.outputs.ready == 'true'",
            finalize_status,
        )
        self.assertIn("conclusion: 'neutral'", aggregate)
        self.assertIn("exactSuccess && workingIsExact ? 'success' : 'failure'", aggregate)
        self.assertIn("conclusion,", aggregate)
        self.assertIn("if (!ownsPendingLease)", finalize_status)
        self.assertIn("if (state !== 'success')", finalize_status)
        self.assertEqual(aggregate_failure_terminalization_errors(aggregate), [])
        reporter_race = finalize_status[
            finalize_status.index("if (!liveTerminal)") : finalize_status.index("if (!ownsPendingLease)")
        ]
        self.assertIn("core.setFailed", reporter_race)
        superseded_race = finalize_status[
            finalize_status.index("if (!ownsPendingLease)") : finalize_status.index("if (state !== 'success')")
        ]
        self.assertIn("core.setFailed", superseded_race)
        self.assertIn("state: 'success'", finalize_status)
        self.assertIn("pendingDescription", finalize_status)
        pending_badge = named_step(
            aggregate, "Keep workflow badge fail-closed until exact evidence is terminal"
        )
        self.assertIn(
            "if: always() && steps.terminal-publication-readiness.outputs.ready != 'true'",
            pending_badge,
        )
        self.assertIn("exit 1", pending_badge)
        self.assertIn("neutralizePublishedCheck", finalize_status)
        self.assertIn("CHECK_ID", finalize_status)
        self.assertIn(
            "current.status === 'completed' && current.conclusion === 'neutral'",
            finalize_status,
        )
        self.assertIn("catch (error)", finalize_status)
        self.assertIn("Aggregate evaluation or publication was uncertain", finalize_status)
        self.assertIn("aggregateSuccessPublished = true", finalize_status)
        self.assertIn("Post-publication reporter identity changed", finalize_status)
        self.assertIn("Post-publication aggregate success identity changed", finalize_status)
        self.assertIn("publishedAggregate.id", finalize_status)
        self.assertIn("aggregate was superseded after publication", finalize_status)
        self.assertIn("state: 'failure'", finalize_status)
        self.assertIn("per_page: 100", aggregate)
        self.assertIn("prior.status === 'completed'", aggregate)
        self.assertIn("['success', 'neutral'].includes(prior.conclusion)", aggregate)
        self.assertIn("action: context.payload.action", readiness)
        self.assertIn("app?.slug !== 'github-actions'", aggregate)
        self.assertIn("details_url", aggregate)
        self.assertIn("/runs/${check?.id}", aggregate)
        self.assertIn("String(check.details_url || '') !== `${detailsPrefix}${check.id}`", aggregate)
        self.assertNotIn("details_url: detailsUrl", aggregate)
        self.assertIn("trusted-ci-final-gate.env", aggregate)
        self.assertIn("listCommitStatusesForRef", aggregate)
        self.assertIn("build_matrix_status_id", aggregate)
        self.assertIn("codeql_status_id", aggregate)
        self.assertIn("currentBadge.status !== 'in_progress'", aggregate)
        self.assertIn("currentBadge.conclusion !== null", aggregate)
        self.assertNotIn("continue-on-error", aggregate)
        self.assertNotIn("|| true", aggregate)

    def test_trusted_aggregate_failure_terminalization_rejects_hostile_mutations(self) -> None:
        aggregate = TRUSTED_CI_AGGREGATE_WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(aggregate_failure_terminalization_errors(aggregate), [])
        mutations = {
            "pending identity validation removed": aggregate.replace(
                "statusContract.validateLatestPendingStatus({",
                "statusContract.hasCurrentPendingLease({",
                1,
            ),
            "failure changed to pending": aggregate.replace(
                "state: 'failure'", "state: 'pending'", 1
            ),
            "published identity detached": aggregate.replace(
                "postFailureMatches[0].id !== publishedFailure.id",
                "postFailureMatches[0].id < 1",
                1,
            ),
            "bot identity weakened": aggregate.replace(
                "postFailureListed.creator?.id !== 41898282",
                "!postFailureListed.creator",
                1,
            ),
            "post-verification rejection removed": aggregate.replace(
                "throw new Error('Post-publication aggregate failure identity changed.');",
                "core.notice('Aggregate failure identity changed.');",
                1,
            ),
            "action fails before publish": aggregate.replace(
                "const publishedFailure = await statusContract.publishValidatedTerminalStatus({",
                "core.setFailed(`Trusted exact-source aggregate status failed: ${outcomes.join(', ')}; Working exact: ${workingIsExact}.`);\n"
                "            const publishedFailure = await statusContract.publishValidatedTerminalStatus({",
                1,
            ),
        }
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, aggregate, "mutation fixture did not alter YAML")
                self.assertTrue(aggregate_failure_terminalization_errors(mutated), label)

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
        self.assertIn("EXACT_BUILD_MATRIX_STATUS_ID: ${{ steps.exact-gate.outputs.build_matrix_status_id }}", self.site_data_publish)
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

        proof = named_step(self.site_data_publish, "Repeat the complete repository proof")
        first_site_generation = proof.index("python3 tools/site-data/generate.py")
        docs_validation = proof.index("python3 tools/site-data/validate.py --docs")
        self.assertGreater(
            docs_validation,
            first_site_generation,
            "clean-checkout docs validation must follow API generation",
        )

    def test_release_controller_cannot_run_from_a_caller_selected_ref(self) -> None:
        header = self.release[: self.release.index("permissions:")]
        self.assertIn("repository_dispatch:", header)
        self.assertNotIn("workflow_dispatch:", header)
        self.assertNotIn("  push:", header)
        controller = named_step(self.release, "Attest current trusted release controller")
        self.assertIn('EVENT_REF" != "refs/heads/Working', controller)
        self.assertIn('LOCAL_SHA" != "$WORKFLOW_SHA', controller)
        self.assertIn('LOCAL_SHA" != "$REMOTE_SHA', controller)

    def test_release_sbom_output_matches_every_consumer(self) -> None:
        self.assertEqual(release_sbom_name_errors(self.release), [])
        consumer = (REPO_ROOT / ".github" / "scripts" / "verify_published_stable_release.py").read_text(encoding="utf-8")
        self.assertIn(f'"{CANONICAL_RELEASE_SBOM}"', consumer)

    def test_release_sbom_name_contract_rejects_drift(self) -> None:
        output_line = f"        output-file: {CANONICAL_RELEASE_SBOM}\n"
        bundle_argument = f'--sbom "$GITHUB_WORKSPACE/{CANONICAL_RELEASE_SBOM}"'
        self.assertIn(output_line, self.release)
        self.assertIn(bundle_argument, self.release)
        mutations = {
            "action writes another name": self.release.replace(
                output_line, "        output-file: SparkEngine.spdx.json\n", 1),
            "bundle verifier reads another name": self.release.replace(
                bundle_argument, '--sbom "$GITHUB_WORKSPACE/SparkEngine.spdx.json"', 1),
        }
        if f"        sbom: {CANONICAL_RELEASE_SBOM}\n" in self.release:
            mutations["scanner reads another name"] = self.release.replace(
                f"        sbom: {CANONICAL_RELEASE_SBOM}\n", "        sbom: SparkEngine.spdx.json\n", 1)
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.release, label)
                self.assertTrue(release_sbom_name_errors(mutated), label)

    def test_release_prepare_runs_supply_chain_policy_before_metadata(self) -> None:
        policy = named_step(self.release, "Supply-chain policy check")
        metadata = named_step(self.release, "Compute release metadata")
        self.assertIn("python3 tools/check-supply-chain.py --ci", policy)
        self.assertNotIn("continue-on-error", policy)
        self.assertNotIn("|| true", policy)
        self.assertLess(self.release.index(policy), self.release.index(metadata))

    def test_release_metadata_requires_one_source_version_and_changelog_entry(self) -> None:
        block = named_step(self.release, "Compute release metadata")
        script = local_release_fixture_script(
            textwrap.dedent(block.split("run: |\n", 1)[1])
        )
        declaration = 'set(SPARK_ENGINE_VERSION "1.2.3" CACHE STRING "Engine version")\n'
        heading = "## [1.2.3] - 2026-09-07\n\n### Fixed\n- Fixture release note.\n"
        cases = (
            ("matching", "v1.2.3", declaration, heading, 0),
            ("heading without date", "v1.2.3", declaration, "## [1.2.3]\n", 0),
            ("version mismatch", "v9.8.7", declaration, "## [9.8.7]\n", 1),
            ("duplicate default", "v1.2.3", declaration * 2, heading, 1),
            ("conflicting default", "v1.2.3", declaration + declaration.replace("1.2.3", "9.8.7"), heading, 1),
            ("multiline duplicate", "v1.2.3", declaration + 'set(\n SPARK_ENGINE_VERSION "9.8.7")\n', heading, 1),
            ("indented duplicate", "v1.2.3", declaration + "  " + declaration, heading, 1),
            ("missing default", "v1.2.3", "", heading, 1),
            ("unreleased only", "v1.2.3", declaration, "## [Unreleased]\n", 1),
            ("missing changelog", "v1.2.3", declaration, None, 1),
            ("duplicate heading", "v1.2.3", declaration, heading * 2, 1),
            ("other release", "v1.2.3", declaration, "## [1.2.30]\n", 1),
            ("nonheading mention", "v1.2.3", declaration, "See [1.2.3] for details.\n", 1),
            ("regex lookalike", "v1.2.3", declaration, "## [1x2x3]\n", 1),
            ("malformed tag", "v1.2.3-rc1", declaration, heading, 1),
        )
        for label, tag, cmake, changelog, status in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                (root / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
                if changelog is not None:
                    (root / "CHANGELOG.md").write_text(changelog, encoding="utf-8")
                output = root / "outputs"
                completed = subprocess.run(
                    [bash_executable(), "-c", script], cwd=root, text=True, capture_output=True,
                    env={**os.environ, "EVENT_NAME": "repository_dispatch", "INPUT_RELEASE_TAG": tag,
                         "GITHUB_OUTPUT": str(output)},
                )
                self.assertEqual(completed.returncode, status, completed.stderr)
                values = dict(line.split("=", 1) for line in output.read_text().splitlines()) if output.exists() else {}
                if status:
                    self.assertNotIn("is_versioned", values, "Rejected metadata must not emit release outputs")
                else:
                    self.assertEqual(values, {"tag": tag, "version": "1.2.3", "cmake_version": "1.2.3",
                                              "is_versioned": "true"})

    def test_nightly_metadata_does_not_require_versioned_changelog(self) -> None:
        block = named_step(self.release, "Compute release metadata")
        script = textwrap.dedent(block.split("run: |\n", 1)[1])
        for event, tag in (("schedule", ""), ("schedule", "v9.8.7"), ("repository_dispatch", "")):
            with self.subTest(event=event, tag=tag), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                (root / "CMakeLists.txt").write_text(
                    'set(SPARK_ENGINE_VERSION "1.2.3" CACHE STRING "Engine version")\n', encoding="utf-8")
                output = root / "outputs"
                completed = subprocess.run(
                    [bash_executable(), "-c", script], cwd=root, text=True, capture_output=True,
                    env={**os.environ, "EVENT_NAME": event, "INPUT_RELEASE_TAG": tag,
                         "GITHUB_OUTPUT": str(output), "GITHUB_RUN_ID": "123",
                         "GITHUB_RUN_ATTEMPT": "1", "GITHUB_SHA": "a" * 40},
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertEqual(dict(line.split("=", 1) for line in output.read_text().splitlines()),
                                 {"tag": "nightly-123-1-aaaaaaaaaaaa", "version": "nightly", "cmake_version": "1.2.3",
                                  "is_versioned": "false"})

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

    def test_windows_package_native_failures_stop_the_step(self) -> None:
        windows = yaml_section(self.release, "build-windows", indent=2)
        for name in ("Validate external package consumption", "Extract and smoke-test portable package",
                     "Validate Windows stable runtime component layout"):
            block = named_step(windows, name)
            lines = textwrap.dedent(block.split("run: |\n", 1)[1]).splitlines()
            native_ends = []
            in_native = False
            for index, line in enumerate(lines):
                if re.match(r"^(cmake|ctest|python)\s", line):
                    in_native = True
                if in_native and not line.rstrip().endswith("`"):
                    native_ends.append(index)
                    in_native = False
            self.assertGreaterEqual(len(native_ends), 4, name)
            for end in native_ends:
                with self.subTest(step=name, command=lines[end]):
                    self.assertLess(end + 1, len(lines), "Native status must be checked immediately")
                    self.assertRegex(lines[end + 1], r"^if \(\$LASTEXITCODE -ne 0\) \{ exit \$LASTEXITCODE \}$")

    def test_release_package_gates_reject_advisory_conditional_or_shell_bypasses(self) -> None:
        mutations = {
            "advisory Linux package job": self.release.replace(
                "  build-linux:\n    needs: [prepare]\n",
                "  build-linux:\n    needs: [prepare]\n    continue-on-error: true\n",
                1,
            ),
            "conditional Linux package smoke": self.release.replace(
                "    - name: Extract and smoke-test portable package\n      run: |\n",
                "    - name: Extract and smoke-test portable package\n      if: always()\n      run: |\n",
                1,
            ),
            "Linux shell error suppression": self.release.replace(
                "    - name: Extract and smoke-test portable package\n      run: |\n        archive_count=",
                "    - name: Extract and smoke-test portable package\n      run: |\n        set +e\n        archive_count=",
                1,
            ),
            "custom Linux shell suppresses failure": self.release.replace(
                "    - name: Extract and smoke-test portable package\n      run: |\n",
                "    - name: Extract and smoke-test portable package\n      shell: bash {0} || true\n      run: |\n",
                1,
            ),
            "Windows package shell suppresses failure": self.release.replace(
                "    - name: Extract and smoke-test portable package\n      shell: pwsh\n",
                "    - name: Extract and smoke-test portable package\n      shell: pwsh -Command {0} || exit 0\n",
                1,
            ),
        }

        self.assertEqual(release_package_gate_errors(self.release), [])
        for label, mutated in mutations.items():
            with self.subTest(mutation=label):
                self.assertNotEqual(mutated, self.release)
                self.assertTrue(release_package_gate_errors(mutated), label)

    def test_native_msi_qualification_blocks_package_upload_and_retains_logs(self) -> None:
        windows = yaml_section(self.release, "build-windows", indent=2)
        block = named_step(windows, "Qualify Windows stable MSI install upgrade rollback repair and uninstall")
        self.assertIn("if: needs.prepare.outputs.is_versioned == 'true'", block)
        self.assertIn("python .github/scripts/qualify-windows-msi.py", block)
        self.assertIn("python .github/scripts/write-shipping-package-manifest.py", block)
        self.assertIn("python .github/scripts/provision-previous-windows-msi.py", block)
        self.assertIn('--repository "${{ github.repository }}"', block)
        self.assertIn('--current-version "${{ needs.prepare.outputs.cmake_version }}"', block)
        self.assertIn("provisioning-receipt.json", block)
        self.assertNotIn("SPARK_PREVIOUS_WINDOWS_PACKAGE_DIR", block)
        self.assertNotIn("SPARK_PREVIOUS_WINDOWS_PACKAGE_MANIFEST", block)
        self.assertIn('--manifest "${{ github.workspace }}/${{ matrix.build_dir }}/SparkEngineGameModules.cmake"', block)
        self.assertIn('$packageManifest = "${{ github.workspace }}/${{ matrix.build_dir }}/packages/shipping-package-manifest.json"', block)
        self.assertIn("--package-manifest $packageManifest", block)
        self.assertIn("--previous-packages $previousPackages", block)
        self.assertIn("--previous-version $previousVersion", block)
        self.assertIn("--previous-package-manifest $previousManifest", block)
        self.assertIn("--previous-receipt $previousReceipt", block)
        self.assertIn('--runner-temp "${{ runner.temp }}"', block)
        self.assertIn('--source-sha "${{ github.sha }}"', block)
        self.assertTrue(block.rstrip().endswith("if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }"))
        self.assertNotIn("continue-on-error", block)
        self.assertLess(windows.index("Generate CPack packages"), windows.index("Qualify Windows stable MSI"))
        self.assertLess(windows.index("Qualify Windows stable MSI"), windows.index("Upload packaged artifact"))
        upload = named_step(windows, "Upload packaged artifact")
        self.assertIn("shipping-package-manifest.json", upload)
        logs = named_step(windows, "Upload native MSI qualification diagnostics")
        self.assertIn("if: always()", logs)
        self.assertIn("path: msi-qualification/", logs)
        self.assertIn("test_qualify_windows_msi.py", self.build)

    def test_shipping_ci_publishes_exact_msi_for_package_smoke_without_rebuilding(self) -> None:
        shipping = yaml_section(self.build, "build-windows-shipping", indent=2)
        block = named_step(shipping, "Package Windows Shipping MSI for package-smoke consumer")
        self.assertIn("cpack --config build/windows-shipping/CPackConfig.cmake -G WIX -C MinSizeRel", block)
        self.assertIn("python .github/scripts/write-shipping-package-manifest.py", block)
        self.assertIn('"build/ci-msi-package"', block)
        self.assertIn('SPARK_ENGINE_VERSION:STRING=', block)
        self.assertIn('--out "$packageRoot/shipping-package-manifest.json"', block)
        self.assertNotIn("qualify-windows-msi.py", block)
        self.assertEqual(block.count("if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }"), 2)
        self.assertNotIn("continue-on-error", block)
        self.assertNotIn("cmake --build", block)
        self.assertNotIn("cmake --preset", block)
        self.assertNotIn("if: ", block)
        self.assertLess(shipping.index("validate_pending_authority.py"), shipping.index("Package Windows Shipping MSI"))
        artifact = named_step(shipping, "Upload exact Windows Shipping MSI for package smoke")
        self.assertIn("shipping-package-${{ github.sha }}", artifact)
        self.assertIn("shipping-package-manifest.json", artifact)
        self.assertIn("SparkEngineGameModules.cmake", artifact)
        self.assertIn("if-no-files-found: error", artifact)
        self.assertIn("msi-cpack.log", block)

    def test_package_smoke_job_consumes_exact_shipping_artifact_and_publishes_evidence(self) -> None:
        smoke = yaml_section(self.build, "module-profile-package-smoke", indent=2)
        self.assertIn("needs: [build-windows-shipping]", smoke)
        self.assertIn("runs-on: windows-2022", smoke)
        self.assertIn("shipping-package-${{ github.sha }}", smoke)
        self.assertIn("qualify-windows-msi.py", smoke)
        self.assertIn("--package-manifest", smoke)
        self.assertIn("module-profile-package-smoke-${{ github.sha }}", smoke)
        self.assertIn("msi-qualification/package-smoke.log", smoke)
        self.assertIn("if-no-files-found: error", smoke)
        self.assertNotIn("continue-on-error", smoke)
        self.assertIn(
            "if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }\n\n"
            "      - name: Upload package smoke evidence",
            smoke,
        )

    def test_required_windows_lane_gates_native_package_evidence_contracts(self) -> None:
        windows = yaml_section(self.build, "build-windows-vs2022", indent=2)
        step = named_step(windows, "Test native Windows package evidence producers")
        self.assertIn("if: matrix.config == 'Release'", step)
        self.assertIn("test_write_shipping_package_manifest.py", step)
        self.assertIn("test_provision_previous_windows_msi.py", step)
        self.assertIn("test_qualify_windows_msi.py", step)
        self.assertNotIn("continue-on-error", step)

    def test_stable_runtime_layout_is_validated_before_packaging(self) -> None:
        windows = yaml_section(self.release, "build-windows", indent=2)
        block = named_step(windows, "Validate Windows stable runtime component layout")
        self.assertIn("if: needs.prepare.outputs.is_versioned == 'true'", block)
        self.assertIn("-DSPARK_PACKAGE_LAYOUT=runtime", block)
        self.assertIn("-DSPARK_PACKAGE_PROFILE=stable-v1", block)
        self.assertIn('-DSPARK_PACKAGE_EXPECTED_MODULE_MANIFEST="${{ github.workspace }}/${{ matrix.build_dir }}/SparkEngineGameModules.cmake"', block)
        for component in ("runtime", "tools", "samples"):
            self.assertIn(f"--component {component}", block)
        self.assertNotIn("--component sdk", block)
        self.assertNotIn("SPARK_PACKAGE_VALIDATE_MODULES_ONLY", block)
        self.assertLess(windows.index("Validate Windows stable runtime component layout"),
                        windows.index("Generate CPack packages"))
        logs = named_step(windows, "Upload runtime layout diagnostics")
        self.assertIn("if: always()", logs)
        self.assertIn("runtime-layout.log", logs)

    def test_stable_shipping_build_keeps_validation_separate(self) -> None:
        windows = yaml_section(self.release, "build-windows", indent=2)
        shipping = named_step(windows, "Configure Windows Shipping package")
        validation = named_step(windows, "Configure Windows stable validation")
        tests = named_step(windows, "Test Windows stable validation")
        for block in (shipping, validation, tests):
            self.assertIn("if: needs.prepare.outputs.is_versioned == 'true'", block)
            self.assertNotIn("continue-on-error", block)
        self.assertIn("cmake --preset windows-shipping", shipping)
        self.assertNotIn("-DBUILD_TESTS", shipping)
        self.assertIn("cmake --preset windows-release", validation)
        self.assertIn("ctest --test-dir build/windows-release -C Release", tests)
        self.assertIn("--no-tests=error", tests)
        package_build = named_step(windows, "Build")
        self.assertIn("cmake --build ${{ matrix.build_dir }} --config ${{ matrix.config }}", package_build)
        download = named_step(self.release, "Download channel build artifacts")
        self.assertIn("needs.prepare.outputs.is_versioned == 'true' && 'SparkEngine-Windows-MinSizeRel-packages'", download)

    def test_release_matrix_selects_shipping_only_for_stable_windows(self) -> None:
        block = named_step(self.release, "Determine build configurations")
        script = textwrap.dedent(block.split("run: |\n", 1)[1])
        bash = bash_executable()
        for stable, requested, status in (
            ("true", "both", 0), ("true", "release", 0), ("true", "debug", 1),
            ("false", "both", 0), ("false", "release", 0), ("false", "debug", 0),
            ("true", "unrecognized", 1),
        ):
            with self.subTest(stable=stable, requested=requested), tempfile.TemporaryDirectory() as raw:
                output = Path(raw) / "outputs"
                completed = subprocess.run(
                    [bash, "-c", script], text=True, capture_output=True,
                    env={**os.environ, "IS_VERSIONED": stable, "REQUESTED_CONFIGS": requested,
                         "EVENT_NAME": "repository_dispatch", "GITHUB_OUTPUT": str(output)},
                )
                self.assertEqual(completed.returncode, status, completed.stderr)
                if status:
                    continue
                values = dict(line.split("=", 1) for line in output.read_text().splitlines())
                self.assertIn("windows_matrix", values, "Windows publication configuration must be explicit")
                windows = json.loads(values["windows_matrix"])
                if stable == "true":
                    self.assertEqual(windows, [{"config": "MinSizeRel", "build_dir": "build/windows-shipping",
                                                "profile": "stable-v1"}])
                else:
                    self.assertEqual([row["config"] for row in windows], ["Debug", "Release"])
                    self.assertTrue(all(row["build_dir"] == "build" and row["profile"] == "default"
                                        for row in windows))
                    self.assertEqual(json.loads(values["configs"]), ["Debug", "Release"])

    def test_stable_collection_excludes_nonshipping_artifacts_and_requires_complete_packages(self) -> None:
        block = named_step(self.release, "Collect release assets")
        script = textwrap.dedent(block.split("run: |\n", 1)[1])
        script = script.replace("${{ needs.prepare.outputs.is_versioned }}", "true")
        names = (
            "SparkEngine-7.8.9-Windows-AMD64-MinSizeRel.zip",
            "SparkEngine-7.8.9-Windows-AMD64-MinSizeRel-Runtime.exe",
            "SparkEngine-7.8.9-Windows-AMD64-MinSizeRel-Runtime.msi",
            "shipping-package-manifest.json",
        )
        for missing in (None, names[0], names[1], names[2], names[3]):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                packages = root / "release-assets/SparkEngine-Windows-MinSizeRel-packages"
                packages.mkdir(parents=True)
                for name in names:
                    if name != missing:
                        (packages / name).write_bytes(b"fixture package")
                extras = root / "release-assets/unrelated"
                extras.mkdir()
                for name in ("SparkInstaller-Windows-x64.exe", "SparkEngine-7.8.9-Linux-x86_64-Release.tar.gz",
                             "SparkEngine-7.8.9-Windows-AMD64-Release.zip"):
                    (extras / name).write_bytes(b"not a Shipping package")
                completed = subprocess.run(
                    [bash_executable(), "-c", script], cwd=root, text=True, capture_output=True,
                    env={**os.environ, "IS_VERSIONED": "true", "RELEASE_VERSION": "7.8.9",
                         "GITHUB_OUTPUT": str(root / "outputs"), "GITHUB_STEP_SUMMARY": str(root / "summary")},
                )
                if missing:
                    self.assertNotEqual(completed.returncode, 0, "Missing stable package must block publication")
                else:
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    assets = (root / "expected-release-assets.txt").read_text().splitlines()
                    self.assertEqual(set(assets), {*names, "SHA256SUMS"})
                    self.assertEqual(assets.count("shipping-package-manifest.json"), 1)

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
        bash = Path(bash_executable())
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
