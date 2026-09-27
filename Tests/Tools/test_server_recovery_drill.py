#!/usr/bin/env python3
"""OPS-110: SparkServer recovery drill (detect, diagnose, drain, restart, recover).

ConfigTests and SummaryTests cover the pure pieces of
tools/ops/server_recovery_drill.py. HarnessTests drive the drill end to end
against a stand-in server that speaks SparkServer's command line and health
contract (atomic health file, one JSON snapshot per stdout line, a stop file
that drains then stops) on every platform, and can be told to never become
ready, fail to come back after a restart, come back as a different build,
come back without ticking, keep its health file changing after it was killed,
skip the draining snapshot, or exit non-zero -- proving each failure mode
fails the drill and that a well-behaved server passes.

RunbookParityTests keep wiki/advanced/Server-Operations-Runbook.md honest:
every SparkServer flag it shows must be in ServerApplication.cpp's usage text,
every tools/ops command must use that script's own flags, every
SparkOrchestrator command must be in its usage text, and every cited path and
page link must exist.

ServerRecoveryDrillProcess drills the real SparkServer with the SparkGame
module when CTest supplies SPARK_SERVER and SPARK_SERVER_MODULE (CTest
Server_RecoveryDrill, label recovery-drill). SPARK_SERVER_DRILL_SUMMARY keeps
the JSON summary and SPARK_SERVER_DRILL_EXPECTED_SHA labels it with an
externally supplied commit.
"""

from __future__ import annotations

import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "ops"
sys.path.insert(0, str(TOOL_DIR))

import server_recovery_drill as drill_tool  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"
OTHER_SHA = "fedcba9876543210fedcba9876543210fedcba98"

# Stand-in SparkServer. It checks the drill's argv, counts its launches in the
# work directory, and publishes the same health JSON SparkServer does.
# DRILL_FAULT selects one misbehaviour.
STAND_IN = """\
import json, os, subprocess, sys, time
args = sys.argv[1:]
def opt(name):
    return args[args.index(name) + 1]
assert "--module" in args and os.path.isfile(opt("--module")), args
assert opt("--bind-address") == "loopback" and "--no-lan-broadcast" in args, args
health_path = opt("--health-file")
stop_path = opt("--stop-file")
interval = int(opt("--status-interval-ms")) / 1000.0
rate = float(opt("--tick-rate"))
fault = os.environ.get("DRILL_FAULT", "")
counter = os.path.join(os.path.dirname(health_path), "stand-in-launches")
launch = (int(open(counter).read()) if os.path.exists(counter) else 0) + 1
open(counter, "w").write(str(launch))
restarted = launch > 1
commit = OTHER_SHA if fault == "restart_wrong_commit" and restarted else SHA

def publish(live, ready, draining, stopping, ticks, echo=True):
    record = {"live": live, "ready": ready, "draining": draining, "stopping": stopping, "port": 1, "players": 0,
              "ticks": ticks, "loadedModules": 1 if live else 0, "gameModule": "Stand-in" if live else "",
              "map": "drill", "error": "", "version": "0.9.0", "commit": commit, "treeState": "clean"}
    text = json.dumps(record, separators=(",", ":"))
    if echo:
        print(text, flush=True)
    temp = health_path + ".tmp." + str(os.getpid())
    with open(temp, "w") as stream:
        stream.write(text + "\\n")
    os.replace(temp, health_path)

if fault == "keeps_writing" and not os.environ.get("DRILL_WRITER"):
    # A detached helper keeps the health file changing after this process is
    # killed, so a detector that trusted the file's mere presence would pass.
    subprocess.Popen([sys.executable, "-B", os.path.abspath(sys.argv[0])] + args,
                     env={**os.environ, "DRILL_WRITER": "1"}, cwd=os.path.dirname(os.path.abspath(sys.argv[0])),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
writer = bool(os.environ.get("DRILL_WRITER"))
start = time.monotonic()
next_status = 0.0
while not os.path.exists(stop_path):
    elapsed = time.monotonic() - start
    if writer and elapsed > WRITER_LIFETIME_S:
        sys.exit(0)
    if fault == "never_ready" or (fault == "no_restart_ready" and restarted):
        time.sleep(0.05)
        continue
    ticks = 7 if fault == "frozen_restart" and restarted else int(elapsed * rate)
    if elapsed >= next_status:
        publish(True, True, False, False, ticks, echo=not writer)
        next_status = elapsed + interval
    time.sleep(1.0 / rate)

if fault != "no_drain":
    publish(True, False, True, False, ticks)
publish(True, False, fault != "no_drain", True, ticks)
publish(False, False, False, False, 0)
sys.exit(3 if fault == "exit_nonzero" else 0)
"""

# Outlives the drill's detection window (3 x stale_after_s after the kill).
WRITER_LIFETIME_S = 3.0
FAST = drill_tool.DrillConfig(status_interval_ms=100, stale_after_s=0.5, startup_timeout_s=5.0, stop_timeout_s=3.0)


class ConfigTests(unittest.TestCase):
    def test_defaults_validate(self) -> None:
        drill_tool.DrillConfig().validate()
        FAST.validate()

    def test_staleness_window_must_cover_three_status_intervals(self) -> None:
        with self.assertRaises(drill_tool.DrillError):
            drill_tool.DrillConfig(status_interval_ms=250, stale_after_s=0.5).validate()

    def test_out_of_range_values_are_rejected(self) -> None:
        for overrides in ({"status_interval_ms": 50}, {"stale_after_s": float("nan")}, {"stop_timeout_s": 0.0},
                          {"tick_rate_hz": 5000.0}, {"startup_timeout_s": -1.0}):
            with self.subTest(overrides=overrides), self.assertRaises(drill_tool.DrillError):
                drill_tool.DrillConfig(**overrides).validate()

    def test_wedge_is_posix_only(self) -> None:
        self.assertEqual("wedge" in drill_tool.supported_scenarios(), os.name == "posix")
        self.assertIn("crash", drill_tool.supported_scenarios())
        self.assertIn("drain", drill_tool.supported_scenarios())


class SummaryTests(unittest.TestCase):
    def test_summary_is_labelled_and_strict_json(self) -> None:
        outcome = drill_tool.DrillOutcome(identity={"commit": SHA}, launches=2)
        outcome.scenarios.append(drill_tool.ScenarioResult("crash", diagnosis="exited", detect_s=0.6,
                                                           recover_s=1.2))
        document = drill_tool.build_summary(outcome, FAST, SHA, "2026-09-27T00:00:00Z")
        json.dumps(document, allow_nan=False)
        self.assertEqual(document["schema"], "spark-server-recovery-drill/1")
        self.assertEqual(document["verdict"], "pass")
        self.assertEqual(document["commitSha"], SHA)
        self.assertIn("not a recorded release drill", document["evidenceScope"])
        self.assertTrue(document["host"]["os"])
        self.assertEqual(document["scenarios"][0]["detectS"], 0.6)
        outcome.failures.append("crash: detect: never stale")
        self.assertEqual(drill_tool.build_summary(outcome, FAST, None, "t")["verdict"], "fail")

    def test_invalid_inputs_fail_before_launch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            module = Path(tmp) / "SparkGame.bin"
            module.write_bytes(b"\0")
            for scenarios, sha in ((["crash"], "not-a-sha"), ([], None), (["reboot"], None)):
                with self.subTest(scenarios=scenarios, sha=sha), self.assertRaises(drill_tool.DrillError):
                    drill_tool.drill([sys.executable, "-c", "raise SystemExit(9)"], module, FAST, scenarios,
                                     expected_sha=sha, summary=None)


class HarnessTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        stand_in = root / "stand_in_server.py"
        stand_in.write_text(f"SHA = {SHA!r}\nOTHER_SHA = {OTHER_SHA!r}\nWRITER_LIFETIME_S = {WRITER_LIFETIME_S!r}\n"
                            + STAND_IN, encoding="utf-8")
        self.launcher = [sys.executable, "-B", str(stand_in)]
        self.module = root / "SparkGame.bin"
        self.module.write_bytes(b"\0")
        self.summary = root / "summary.json"
        self.work = root / "work"
        self._saved = os.environ.get("DRILL_FAULT")

    def tearDown(self) -> None:
        if self._saved is None:
            os.environ.pop("DRILL_FAULT", None)
        else:
            os.environ["DRILL_FAULT"] = self._saved
        self._tmp.cleanup()

    def run_drill(self, fault: str = "", scenarios: tuple[str, ...] | None = None,
                  expected_sha: str | None = SHA) -> dict:
        os.environ["DRILL_FAULT"] = fault
        outcome, document = drill_tool.drill(self.launcher, self.module, FAST,
                                             scenarios or drill_tool.supported_scenarios(),
                                             expected_sha=expected_sha, summary=self.summary, work_dir=self.work)
        self.assertEqual(json.loads(self.summary.read_text(encoding="utf-8")), document)
        self.assertEqual(document["failures"], outcome.failures)
        return document

    def assertFailsWith(self, document: dict, fragment: str) -> None:
        self.assertEqual(document["verdict"], "fail")
        self.assertTrue(any(fragment in failure for failure in document["failures"]), document["failures"])

    def test_well_behaved_server_passes_every_scenario(self) -> None:
        document = self.run_drill()
        self.assertEqual(document["failures"], [])
        self.assertEqual(document["verdict"], "pass")
        names = [scenario["name"] for scenario in document["scenarios"]]
        self.assertEqual(names, list(drill_tool.supported_scenarios()) + ["final-drain"])
        for scenario in document["scenarios"]:
            self.assertEqual(scenario["status"], "pass", scenario)
        crash = document["scenarios"][0]
        self.assertEqual(crash["diagnosis"], "exited")
        # The killed server's file still claims live=true: detection had to
        # come from staleness, never from the file saying the server is down.
        self.assertIs(crash["lastSnapshotLive"], True)
        self.assertGreaterEqual(crash["detectS"], FAST.stale_after_s)
        self.assertGreater(crash["recoverS"], crash["detectS"])
        drain = next(scenario for scenario in document["scenarios"] if scenario["name"] == "drain")
        self.assertEqual(drain["exitStatus"], 0)
        self.assertEqual(document["launches"], len(drill_tool.supported_scenarios()) + 1)
        self.assertEqual(document["server"]["commit"], SHA)

    @unittest.skipUnless(os.name == "posix", "SIGSTOP wedge is POSIX only")
    def test_wedge_is_diagnosed_as_alive_but_stale(self) -> None:
        document = self.run_drill(scenarios=("wedge",))
        self.assertEqual(document["verdict"], "pass", document["failures"])
        self.assertEqual(document["scenarios"][0]["diagnosis"], "wedged")

    def test_never_ready_fails_startup(self) -> None:
        self.assertFailsWith(self.run_drill("never_ready", ("crash",)), "startup: no fresh live+ready")

    def test_restart_that_never_becomes_ready_fails(self) -> None:
        self.assertFailsWith(self.run_drill("no_restart_ready", ("crash",)), "crash: restart: no fresh live+ready")

    def test_restart_reporting_another_build_fails(self) -> None:
        self.assertFailsWith(self.run_drill("restart_wrong_commit", ("crash",)), "crash: identity:")

    def test_restart_that_does_not_tick_fails(self) -> None:
        self.assertFailsWith(self.run_drill("frozen_restart", ("drain",)), "drain: restart: ticks did not advance")

    def test_health_that_keeps_changing_after_kill_is_not_detected(self) -> None:
        self.assertFailsWith(self.run_drill("keeps_writing", ("crash",)), "crash: detect: health file kept changing")
        # Let the detached writer finish before the work directory is removed.
        health = self.work / "server-health.json"
        deadline = time.monotonic() + WRITER_LIFETIME_S + 5.0
        last = None
        quiet_since = time.monotonic()
        while time.monotonic() < deadline:
            current = health.stat().st_mtime_ns if health.exists() else None
            if current != last:
                last, quiet_since = current, time.monotonic()
            elif time.monotonic() - quiet_since > 1.0:
                break
            time.sleep(0.05)

    def test_missing_drain_snapshot_fails(self) -> None:
        self.assertFailsWith(self.run_drill("no_drain", ("drain",)), "drain: drain: no live+draining snapshot")

    def test_nonzero_exit_after_drain_fails(self) -> None:
        self.assertFailsWith(self.run_drill("exit_nonzero", ("drain",)), "drain: stop: server exited with status 3")

    def test_commit_mismatch_with_expected_sha_fails(self) -> None:
        self.assertFailsWith(self.run_drill(scenarios=("crash",), expected_sha=OTHER_SHA), "identity: server reports")

    def test_cli_exit_status_and_stdout_summary(self) -> None:
        server = Path(self._tmp.name) / "SparkServer.missing"
        stderr = io.StringIO()
        with redirect_stdout(io.StringIO()), redirect_stderr(stderr):
            self.assertEqual(drill_tool.main(["--server", str(server), "--module", str(self.module)]), 1)
        self.assertIn("is not a file", stderr.getvalue())


RUNBOOK = REPO_ROOT / "wiki" / "advanced" / "Server-Operations-Runbook.md"
SERVER_USAGE_SOURCE = REPO_ROOT / "SparkServer" / "src" / "ServerApplication.cpp"
ORCHESTRATOR_USAGE_SOURCE = REPO_ROOT / "SparkDaemon" / "src" / "OrchestratorMain.cpp"
FLAG_RE = re.compile(r"(?<![\w-])--[a-z][a-z0-9-]*")
REPO_PATH_RE = re.compile(r"\b(?:SparkServer|SparkDaemon|GameModules|tools|Tests|docs)/[\w./-]*\w\.(?:py|cpp|h|md|json)\b")


def _runbook_commands(text: str) -> list[str]:
    """Every code-block line and inline code span in the runbook."""
    commands: list[str] = []
    in_block = False
    for line in text.splitlines():
        if line.startswith("```"):
            in_block = not in_block
        elif in_block:
            commands.append(line.strip())
        else:
            commands.extend(span.strip() for span in re.findall(r"`([^`]+)`", line))
    return [command for command in commands if command]


def _script_flags(script: Path) -> set[str]:
    return set(re.findall(r"add_argument\(\s*\"(--[a-z0-9-]+)\"", script.read_text(encoding="utf-8")))


class RunbookParityTests(unittest.TestCase):
    """wiki/advanced/Server-Operations-Runbook.md may only cite operator surfaces that exist."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RUNBOOK.read_text(encoding="utf-8")
        cls.commands = _runbook_commands(cls.text)
        usage = SERVER_USAGE_SOURCE.read_text(encoding="utf-8")
        cls.server_flags = set(re.findall(r"^\s*\"\s+(--[a-z][a-z0-9-]*)", usage, re.MULTILINE))
        orchestrator = ORCHESTRATOR_USAGE_SOURCE.read_text(encoding="utf-8")
        usage_text = orchestrator[orchestrator.index("void PrintUsage()"):orchestrator.index("} // namespace")]
        cls.orchestrator_commands = set(re.findall(r"\b[a-z][a-z-]+\b", usage_text))

    def test_server_usage_is_parsed(self) -> None:
        self.assertTrue({"--module", "--health-file", "--stop-file", "--version"} <= self.server_flags,
                        self.server_flags)

    def test_sparkserver_flags_exist(self) -> None:
        server_commands = [command for command in self.commands if command.split()[0] == "SparkServer"]
        self.assertGreaterEqual(len(server_commands), 3, "runbook no longer shows SparkServer invocations")
        for command in server_commands:
            with self.subTest(command=command):
                self.assertLessEqual(set(FLAG_RE.findall(command)), self.server_flags)

    def test_script_commands_use_existing_flags(self) -> None:
        checked = 0
        for command in self.commands:
            match = re.search(r"\btools/ops/\w+\.py\b", command)
            if not match or not FLAG_RE.search(command):
                continue
            script = REPO_ROOT / match.group(0)
            with self.subTest(command=command):
                self.assertTrue(script.is_file(), script)
                self.assertLessEqual(set(FLAG_RE.findall(command)), _script_flags(script))
                checked += 1
        self.assertGreaterEqual(checked, 4, "runbook no longer shows drill commands")

    def test_bare_flags_belong_to_the_server_or_the_drill(self) -> None:
        known = self.server_flags | _script_flags(TOOL_DIR / "server_recovery_drill.py")
        for command in self.commands:
            if FLAG_RE.fullmatch(command.split()[0]):
                with self.subTest(command=command):
                    self.assertIn(command.split()[0], known)

    def test_orchestrator_commands_exist(self) -> None:
        orchestrator_commands = [command for command in self.commands if command.startswith("SparkOrchestrator ")]
        self.assertGreaterEqual(len(orchestrator_commands), 3, "runbook no longer shows SparkOrchestrator commands")
        for command in orchestrator_commands:
            with self.subTest(command=command):
                self.assertIn(command.split()[1], self.orchestrator_commands)

    def test_cited_paths_and_links_exist(self) -> None:
        cited = set(REPO_PATH_RE.findall(self.text))
        self.assertIn("tools/ops/server_recovery_drill.py", cited)
        for path in sorted(cited):
            with self.subTest(path=path):
                self.assertTrue((REPO_ROOT / path).is_file(), path)
        for link in re.findall(r"\]\(([^)#]+\.md)\)", self.text):
            with self.subTest(link=link):
                self.assertTrue((RUNBOOK.parent / link).is_file(), link)


class ServerRecoveryDrillProcess(unittest.TestCase):
    """Real SparkServer drill; registered as CTest Server_RecoveryDrill (label recovery-drill)."""

    def test_real_server_recovery_drill(self) -> None:
        server_env = os.environ.get("SPARK_SERVER")
        if not server_env:
            if os.environ.get("SPARK_SERVER_DRILL_REQUIRED") == "1":
                self.fail("SPARK_SERVER is not set but the real-server drill is required")
            self.skipTest("SPARK_SERVER not set (run through CTest Server_RecoveryDrill)")
        server = Path(server_env)
        module = Path(os.environ["SPARK_SERVER_MODULE"])
        self.assertTrue(server.is_file(), server)
        self.assertTrue(module.is_file(), module)
        expected_sha = os.environ.get("SPARK_SERVER_DRILL_EXPECTED_SHA") or None

        with tempfile.TemporaryDirectory() as tmp:
            summary_path = Path(os.environ.get("SPARK_SERVER_DRILL_SUMMARY") or Path(tmp) / "server-drill.json")
            command = [sys.executable, "-B", str(TOOL_DIR / "server_recovery_drill.py"), "--server", str(server),
                       "--module", str(module), "--summary", str(summary_path)]
            if expected_sha:
                command += ["--expected-sha", expected_sha]
            run = subprocess.run(command, capture_output=True, text=True, timeout=160)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            summary = json.loads(summary_path.read_text(encoding="utf-8"))

        self.assertEqual(summary["verdict"], "pass")
        self.assertEqual(summary["failures"], [])
        names = [scenario["name"] for scenario in summary["scenarios"]]
        self.assertEqual(names, list(drill_tool.supported_scenarios()) + ["final-drain"])
        self.assertRegex(summary["server"]["commit"], r"^([0-9a-f]{40}|unknown)$")
        for scenario in summary["scenarios"]:
            print(f"recovery drill {scenario['name']}: diagnosis={scenario['diagnosis']} "
                  f"detect={scenario['detectS']} stop={scenario['stopS']} recover={scenario['recoverS']}",
                  file=sys.stderr)


if __name__ == "__main__":
    unittest.main()
