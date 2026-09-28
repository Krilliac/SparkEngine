#!/usr/bin/env python3
"""DATA-120 / OPS-110 / PERF-100: the scheduled operations workflow keeps its rehearsal contract.

.github/workflows/operations-scheduled.yml is the only place the recovery
drill, the server soak and the NullRHI soak run on a schedule. These tests
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
REQUIRED_JOBS = ("recovery-drill", "server-soak", "soak-scheduled")
RELEASE_SMOKE_S = 1800
NULLRHI_SCENE_S = 3600


def _commands(step: dict[str, Any]) -> list[str]:
    """Shell commands of a run step, with backslash continuations joined."""
    script = step.get("run")
    if not isinstance(script, str):
        return []
    return [line.strip() for line in script.replace("\\\n", " ").splitlines() if line.strip()]


def _durations(command: str) -> list[int]:
    return [int(value) for value in re.findall(r"--duration\s+(\d+)\b", command)]


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
            "short NullRHI soak": ("--duration 3600", "--duration 1200", "headless-soak-1h"),
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
