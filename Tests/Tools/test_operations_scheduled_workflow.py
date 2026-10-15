#!/usr/bin/env python3
"""DATA-120 / OPS-110 / PERF-100 / HEAD-220 / TF-110: the scheduled operations workflow keeps its contract.

.github/workflows/operations-scheduled.yml is the only place the recovery
drill, the server soak, the Linux and Windows NullRHI soaks, the observability
canary and the Terrafront 30-minute soak run on a schedule. These tests
parse it and fail when a change would let a scheduled run pass without
rehearsing anything: a lost cron trigger, an advisory (continue-on-error)
job, a ctest selection that can match nothing, a soak shorter than its
criterion, a harness not bound to the exact commit, a checkout of anything
but that commit, or evidence that is not uploaded. Each rule is also proven
against a hostile copy of the workflow, so a rule that stops checking fails
here too.

A pass proves the schedule is written down correctly. It is not evidence
that any scheduled run happened; that needs the hosted run history.
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools"))

from buildmatrix.workflow import parse_workflow_yaml  # noqa: E402

WORKFLOW = ROOT / ".github" / "workflows" / "operations-scheduled.yml"
EXACT_SHA = '--expected-sha "${{ github.sha }}"'
REQUIRED_JOBS = ("recovery-drill", "server-soak", "soak-scheduled", "headless-windows-soak",
                 "observability-canary", "terrafront-soak")
RELEASE_SMOKE_S = 1800
NULLRHI_SCENE_S = 3600
TERRAFRONT_MIN_TIMEOUT_MIN = 45
TERRAFRONT_SELECT = ("ctest --test-dir build/linux-gcc-release --show-only=json-v1 --no-tests=error "
                     "-L '^terrafront-soak-long$' > terrafront-soak-long-selection.json")
TERRAFRONT_CHECK = ("python3 Tools/Terrafront/check_selection.py terrafront-soak-long-selection.json "
                    "--label terrafront-soak-long")
TERRAFRONT_RUN_PREFIX = "ctest --test-dir build/linux-gcc-release -L '^terrafront-soak-long$'"


def _commands(step: dict[str, Any]) -> list[str]:
    """Shell commands of a run step, with backslash continuations joined."""
    script = step.get("run")
    if not isinstance(script, str):
        return []
    return [line.strip() for line in script.replace("\\\n", " ").splitlines() if line.strip()]


def _durations(command: str) -> list[int]:
    return [int(value) for value in re.findall(r"--duration\s+(\d+)\b", command)]


def _job_commands(job: dict[str, Any]) -> list[str]:
    return [command for step in job.get("steps") or [] for command in _commands(step)]


def _windows_soak_errors(job: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if job.get("runs-on") != "windows-2022":
        errors.append(f"headless-windows-soak must run on windows-2022, not {job.get('runs-on')!r}")
    commands = _job_commands(job)
    if not any(command.startswith("cmake --preset windows-shipping") for command in commands) or \
            not any(command.startswith("cmake --build --preset windows-shipping") for command in commands):
        errors.append("headless-windows-soak does not configure and build the windows-shipping preset")
    soak = [command for command in commands if "tools/perf-budget/run_nullrhi_soak.py" in command]
    if not soak:
        errors.append("headless-windows-soak does not run run_nullrhi_soak.py")
    for command in soak:
        durations = _durations(command)
        if len(durations) != 1 or durations[0] < NULLRHI_SCENE_S:
            errors.append(f"Windows NullRHI soak is shorter than the {NULLRHI_SCENE_S} s scene: {command}")
        if EXACT_SHA not in command or "--report" not in command:
            errors.append(f"Windows NullRHI soak is not bound to the exact commit or writes no --report: {command}")
        if "--out" in command:
            errors.append(f"Windows NullRHI soak passes --out, which the harness refuses on Windows: {command}")
    return errors


def _canary_errors(job: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    commands = _job_commands(job)
    if not any("--preset linux-gcc-release" in command for command in commands):
        errors.append("observability-canary does not use linux-gcc-release")
    if any("linux-shipping" in command for command in commands):
        errors.append("observability-canary must not use linux-shipping")
    for label in ("observability", "server-soak"):
        selected = [command for command in commands if command.startswith("ctest ") and f"-L '^{label}$'" in command]
        if not selected:
            errors.append(f"observability-canary does not run ctest -L '^{label}$'")
        for command in selected:
            if "--no-tests=error" not in command:
                errors.append(f"observability-canary ctest can pass without running a {label} test: {command}")
    soak_steps = [step for step in job.get("steps") or []
                  if any(command.startswith("ctest ") and "-L '^server-soak$'" in command
                         for command in _commands(step))]
    if not any((step.get("env") or {}).get("SPARK_SERVER_SOAK_EXPECTED_SHA") == "${{ github.sha }}"
               for step in soak_steps):
        errors.append("observability-canary server soak is not bound to the exact commit")
    return errors


def _terrafront_errors(job: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    timeout = job.get("timeout-minutes")
    if not isinstance(timeout, int) or timeout < TERRAFRONT_MIN_TIMEOUT_MIN:
        errors.append(f"terrafront-soak timeout {timeout!r} cannot hold a 30-minute soak "
                      f"(need >= {TERRAFRONT_MIN_TIMEOUT_MIN} min)")
    commands = _job_commands(job)
    if not any("--preset linux-gcc-release" in command and "-DSPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS=ON" in command
               for command in commands):
        errors.append("terrafront-soak does not configure linux-gcc-release with "
                      "-DSPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS=ON")
    runs = [index for index, command in enumerate(commands) if command.startswith(TERRAFRONT_RUN_PREFIX)]
    if len(runs) != 1 or "--no-tests=error" not in commands[runs[0]]:
        errors.append("terrafront-soak must run the terrafront-soak-long label once with --no-tests=error")
    for label, command in (("selection", TERRAFRONT_SELECT), ("selection check", TERRAFRONT_CHECK)):
        if commands.count(command) != 1:
            errors.append(f"terrafront-soak must pin the long-soak {label} exactly once: {command}")
    if len(runs) == 1 and commands.count(TERRAFRONT_SELECT) == 1 and commands.count(TERRAFRONT_CHECK) == 1:
        if not commands.index(TERRAFRONT_SELECT) < commands.index(TERRAFRONT_CHECK) < runs[0]:
            errors.append("terrafront-soak must select, check, then run the long soak, in that order")
    return errors


def workflow_policy_errors(text: str) -> list[str]:
    """Every way the workflow text breaks the scheduled-operations contract."""
    errors: list[str] = []
    document = parse_workflow_yaml(text)
    triggers = document.get("on") or {}
    schedule = triggers.get("schedule") if isinstance(triggers, dict) else None
    if not isinstance(schedule, list) or not any(isinstance(entry, dict) and entry.get("cron") for entry in schedule):
        errors.append("no schedule cron trigger")
    if not isinstance(triggers, dict) or "workflow_dispatch" not in triggers:
        errors.append("no workflow_dispatch trigger")

    jobs = document.get("jobs") or {}
    for name in REQUIRED_JOBS:
        if name not in jobs:
            errors.append(f"job {name} is missing")
    for name, job in jobs.items():
        steps = job.get("steps") or []
        if "continue-on-error" in job:
            errors.append(f"job {name} is advisory (continue-on-error)")
        checkouts = [step for step in steps if str(step.get("uses", "")).startswith("actions/checkout@")]
        if not checkouts:
            errors.append(f"job {name} has no checkout")
        for step in checkouts:
            if (step.get("with") or {}).get("ref") != "${{ github.sha }}":
                errors.append(f"job {name} does not check out the exact commit")
        uploads = [step for step in steps if str(step.get("uses", "")).startswith("actions/upload-artifact@")]
        if not any(step.get("if") == "always()" and (step.get("with") or {}).get("if-no-files-found") == "error"
                   for step in uploads):
            errors.append(f"job {name} does not always upload its evidence (if-no-files-found: error)")
        for step in steps:
            for command in _commands(step):
                if re.search(r"\|\|\s*(true|exit 0)|\bset \+e\b", command):
                    errors.append(f"job {name} step {step.get('name')!r} suppresses a failure")
            if step.get("continue-on-error") and _commands(step):
                errors.append(f"job {name} step {step.get('name')!r} is advisory")

    drill = jobs.get("recovery-drill") or {}
    runners = {entry.get("os") for entry in ((drill.get("strategy") or {}).get("matrix") or {}).get("include", [])}
    if not {"ubuntu-24.04", "windows-2022"} <= runners:
        errors.append(f"recovery-drill does not run on both Linux and Windows: {sorted(map(str, runners))}")
    drill_ctest = [command for step in drill.get("steps") or [] for command in _commands(step)
                   if command.startswith("ctest ")]
    if len(drill_ctest) < 2:
        errors.append("recovery-drill does not run ctest on each host")
    for command in drill_ctest:
        if "-L recovery-drill" not in command or "--no-tests=error" not in command:
            errors.append(f"recovery-drill ctest can pass without running a drill: {command}")

    soak = [command for step in (jobs.get("server-soak") or {}).get("steps") or [] for command in _commands(step)
            if "tools/ops/server_soak.py" in command]
    soak_durations = sorted(duration for command in soak for duration in _durations(command))
    if len(soak_durations) != len(soak) or not soak:
        errors.append("every server_soak.py run must pass one --duration")
    if RELEASE_SMOKE_S not in soak_durations:
        errors.append(f"no {RELEASE_SMOKE_S} s release smoke")
    if not any(duration > RELEASE_SMOKE_S for duration in soak_durations):
        errors.append("no longer scheduled server soak")
    if any(duration < RELEASE_SMOKE_S for duration in soak_durations):
        errors.append(f"a server soak is shorter than {RELEASE_SMOKE_S} s")
    for command in soak:
        if EXACT_SHA not in command or "--summary" not in command:
            errors.append(f"server soak is not bound to the exact commit or writes no summary: {command}")

    nullrhi = [command for step in (jobs.get("soak-scheduled") or {}).get("steps") or [] for command in _commands(step)
               if "tools/perf-budget/run_nullrhi_soak.py" in command]
    if not nullrhi:
        errors.append("soak-scheduled does not run run_nullrhi_soak.py")
    for command in nullrhi:
        durations = _durations(command)
        if len(durations) != 1 or durations[0] < NULLRHI_SCENE_S:
            errors.append(f"NullRHI soak is shorter than the {NULLRHI_SCENE_S} s headless-soak-1h scene: {command}")
        if EXACT_SHA not in command or "--out" not in command:
            errors.append(f"NullRHI soak records no exact-commit result: {command}")
    errors.extend(_windows_soak_errors(jobs.get("headless-windows-soak") or {}))
    errors.extend(_canary_errors(jobs.get("observability-canary") or {}))
    errors.extend(_terrafront_errors(jobs.get("terrafront-soak") or {}))
    return errors


class OperationsScheduledWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        # A Windows checkout may carry CRLF; the mutation anchors are written with LF.
        cls.text = WORKFLOW.read_text(encoding="utf-8").replace("\r\n", "\n")

    def test_workflow_satisfies_the_contract(self) -> None:
        self.assertEqual(workflow_policy_errors(self.text), [])

    def test_hostile_variants_are_refused(self) -> None:
        cases = {
            "schedule removed": ("  schedule:\n    # Nightly", "  push:\n    # Nightly", "no schedule cron"),
            "advisory job": ("  server-soak:\n    name:", "  server-soak:\n    continue-on-error: true\n    name:",
                             "advisory"),
            "empty ctest selection": ("-L recovery-drill --output-on-failure --no-tests=error \\\n          "
                                      "--output-junit \"$GITHUB_WORKSPACE/ops-evidence/recovery-drill-junit.xml\" "
                                      "2>&1 | tee recovery-drill.log\n\n    - name: Run the recovery drill (Windows)",
                                      "-L recovery-drill --output-on-failure \\\n          "
                                      "--output-junit \"$GITHUB_WORKSPACE/ops-evidence/recovery-drill-junit.xml\" "
                                      "2>&1 | tee recovery-drill.log\n\n    - name: Run the recovery drill (Windows)",
                                      "can pass without running a drill"),
            "short release smoke": ("--duration 1800", "--duration 600", "shorter than 1800"),
            "short NullRHI soak": ("libSparkGame.so --duration 3600", "libSparkGame.so --duration 1200",
                                   "headless-soak-1h"),
            "unbound NullRHI soak": ("--expected-sha \"${{ github.sha }}\" --out", "--out",
                                     "no exact-commit result"),
            "moving checkout": ("        ref: ${{ github.sha }}\n        submodules: recursive\n"
                                "        persist-credentials: false\n\n    - name: Install dependencies\n"
                                "      run: |\n        sudo apt-get update\n        sudo apt-get install -y "
                                "build-essential cmake libgl-dev libvulkan-dev glslang-tools libsdl2-dev "
                                "libfreetype-dev\n\n    - name: Configure Linux Shipping\n      run: |\n"
                                "        set -o pipefail\n        cmake --preset linux-shipping 2>&1 | tee "
                                "server-soak-configure.log",
                                "        ref: Working\n        submodules: recursive\n"
                                "        persist-credentials: false\n\n    - name: Install dependencies\n"
                                "      run: |\n        sudo apt-get update\n        sudo apt-get install -y "
                                "build-essential cmake libgl-dev libvulkan-dev glslang-tools libsdl2-dev "
                                "libfreetype-dev\n\n    - name: Configure Linux Shipping\n      run: |\n"
                                "        set -o pipefail\n        cmake --preset linux-shipping 2>&1 | tee "
                                "server-soak-configure.log",
                                "exact commit"),
            "suppressed soak failure": ("--work-dir ops-evidence/server-soak-work\n\n    - name: Run the weekly",
                                        "--work-dir ops-evidence/server-soak-work || true\n\n    - name: Run the "
                                        "weekly", "suppresses a failure"),
            "evidence not uploaded on failure": ("    - name: Upload NullRHI soak result\n      if: always()\n",
                                                 "    - name: Upload NullRHI soak result\n      if: success()\n",
                                                 "does not always upload"),
            "Windows soak on a Linux runner": ("    runs-on: windows-2022\n    timeout-minutes: 300",
                                               "    runs-on: ubuntu-24.04\n    timeout-minutes: 300",
                                               "must run on windows-2022"),
            "short Windows soak": ("--duration 3600 \\\n          --expected-sha \"${{ github.sha }}\" --report",
                                   "--duration 600 \\\n          --expected-sha \"${{ github.sha }}\" --report",
                                   "Windows NullRHI soak is shorter"),
            "Windows soak writes --out": ("--expected-sha \"${{ github.sha }}\" --report",
                                          "--expected-sha \"${{ github.sha }}\" --out x.json --report",
                                          "refuses on Windows"),
            "Windows soak not bound to the commit": ("--expected-sha \"${{ github.sha }}\" --report", "--report",
                                                     "not bound to the exact commit or writes no --report"),
            "canary can match no tests": ("-L '^observability$' --no-tests=error", "-L '^observability$'",
                                          "without running a observability test"),
            "canary server soak unbound": ("        SPARK_SERVER_SOAK_EXPECTED_SHA: ${{ github.sha }}\n", "",
                                           "server soak is not bound to the exact commit"),
            "canary on the shipping preset": ("cmake --build build/linux-gcc-release --target SparkTests SparkServer",
                                              "cmake --build --preset linux-shipping --target SparkTests SparkServer",
                                              "must not use linux-shipping"),
            "Terrafront timeout too short": ("name: \"Terrafront 30-minute soak\"\n    runs-on: ubuntu-24.04\n"
                                             "    timeout-minutes: 90",
                                             "name: \"Terrafront 30-minute soak\"\n    runs-on: ubuntu-24.04\n"
                                             "    timeout-minutes: 30", "cannot hold a 30-minute soak"),
            "Terrafront selection not checked": ("python3 Tools/Terrafront/check_selection.py "
                                                 "terrafront-soak-long-selection.json --label terrafront-soak-long",
                                                 "true", "pin the long-soak selection check"),
            "Terrafront run can match nothing": ("-L '^terrafront-soak-long$' \\\n          --no-tests=error "
                                                 "--output-on-failure", "-L '^terrafront-soak-long$' \\\n          "
                                                 "--output-on-failure", "once with --no-tests=error"),
            "Terrafront tests not enabled": ("          -DSPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS=ON 2>&1 | tee "
                                             "terrafront-soak-configure.log",
                                             "          2>&1 | tee terrafront-soak-configure.log",
                                             "-DSPARK_ENABLE_TERRAFRONT_MULTICLIENT_TESTS=ON"),
            "Terrafront soak job removed": ("  terrafront-soak:\n    name:", "  terrafront-long:\n    name:",
                                            "job terrafront-soak is missing"),
            "Linux-only drill": ("          - os: windows-2022\n", "          - os: ubuntu-22.04\n",
                                 "both Linux and Windows"),
        }
        for label, (old, new, fragment) in cases.items():
            with self.subTest(label):
                self.assertEqual(self.text.count(old), 1, f"mutation anchor for {label!r} must match once")
                errors = workflow_policy_errors(self.text.replace(old, new))
                self.assertTrue(any(fragment in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
