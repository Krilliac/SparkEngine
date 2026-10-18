#!/usr/bin/env python3
"""OPS-110: tests for tools/ops/evaluate_server_alerts.py (runbook alerts from one health snapshot).

EvaluatorTests drive evaluate_alerts and build_report with real parsed
spark-server-health/1 snapshots and the shipped budget file: a healthy snapshot
fires nothing, and each alert fires exactly on its own condition and clears
when the condition ends. CliTests run the script as a subprocess and pin the
exit-status contract (0 clear, 2 firing, 1 unreadable or contract violation).
Registered with CTest as OpsAlerts_Evaluator.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = REPO_ROOT / "tools" / "ops"
sys.path.insert(0, str(TOOL_DIR))

import evaluate_server_alerts as alerts_tool  # noqa: E402
import server_soak  # noqa: E402
from validate_server_health import parse_health  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"


def snapshot(**overrides: object) -> dict:
    record = {"schema": "spark-server-health/1", "live": True, "ready": True, "draining": False, "stopping": False,
              "port": 1, "players": 0, "ticks": 600, "loadedModules": 1, "gameModule": "SparkGame", "map": "m",
              "error": "", "version": "0.9.0", "commit": SHA, "treeState": "clean", "tickSamples": 600,
              "tickP50Us": 50, "tickP95Us": 500, "tickP99Us": 700, "tickMaxUs": 900, "rssBytes": 1 << 20,
              "netQueueIn": 0, "netQueueOut": 0, "netQueueInPeak": 3, "netQueueOutPeak": 5}
    record.update(overrides)
    return record


def parsed(**overrides: object) -> dict:
    return parse_health(json.dumps(snapshot(**overrides)).encode("utf-8"))


class EvaluatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.budgets = server_soak.load_budgets()

    def firing(self, **overrides: object) -> list[str]:
        return alerts_tool.firing_names(alerts_tool.evaluate_alerts(parsed(**overrides), self.budgets))

    def test_healthy_snapshot_fires_nothing(self) -> None:
        alerts = alerts_tool.evaluate_alerts(parsed(), self.budgets)
        self.assertEqual(alerts_tool.firing_names(alerts), [])
        self.assertEqual([alert.name for alert in alerts],
                         ["server-down", "not-serving", "tick-p95-over-budget", "tick-p99-over-budget",
                          "queue-peak-at-cap"])

    def test_stopped_server_fires_server_down(self) -> None:
        self.assertEqual(self.firing(live=False, ready=False), ["server-down"])

    def test_unready_server_fires_not_serving_but_a_drain_does_not(self) -> None:
        self.assertEqual(self.firing(ready=False), ["not-serving"])
        self.assertEqual(self.firing(ready=False, draining=True), [])

    def test_tick_latency_fires_exactly_above_the_budget(self) -> None:
        p95, p99 = int(self.budgets.values["tickP95Us"]), int(self.budgets.values["tickP99Us"])
        self.assertEqual(self.firing(tickP95Us=p95, tickP99Us=p99, tickMaxUs=p99), [])
        self.assertEqual(self.firing(tickP95Us=p95 + 1, tickP99Us=p99, tickMaxUs=p99), ["tick-p95-over-budget"])
        self.assertEqual(self.firing(tickP99Us=p99 + 1, tickMaxUs=p99 + 1), ["tick-p99-over-budget"])

    def test_percentiles_without_samples_do_not_fire(self) -> None:
        self.assertEqual(self.firing(tickSamples=0, tickP95Us=99999, tickP99Us=99999, tickMaxUs=99999), [])

    def test_queue_peak_fires_at_the_cap_not_below(self) -> None:
        cap = int(self.budgets.values["netQueueCap"])
        self.assertEqual(self.firing(netQueueOutPeak=cap - 1), [])
        self.assertEqual(self.firing(netQueueOutPeak=cap), ["queue-peak-at-cap"])
        self.assertEqual(self.firing(netQueueInPeak=cap), ["queue-peak-at-cap"])

    def test_alert_fires_then_clears_as_the_condition_ends(self) -> None:
        sequence = [parsed(), parsed(tickP99Us=50000, tickMaxUs=50000), parsed()]
        states = [alerts_tool.firing_names(alerts_tool.evaluate_alerts(health, self.budgets)) for health in sequence]
        self.assertEqual(states, [[], ["tick-p99-over-budget"], []])

    def test_budget_overrides_move_the_threshold(self) -> None:
        tight = server_soak.SoakBudgets(version="t", sha256="0" * 64, path="t.json",
                                        values={**self.budgets.values, "tickP99Us": 100}, rationales={})
        self.assertEqual(alerts_tool.firing_names(alerts_tool.evaluate_alerts(parsed(), tight)),
                         ["tick-p99-over-budget"])

    def test_report_is_strict_json_and_names_the_budget_file(self) -> None:
        report = alerts_tool.build_report(parsed(ready=False), self.budgets, "h.json")
        json.dumps(report, allow_nan=False)
        self.assertEqual(report["schema"], "spark-server-alerts/1")
        self.assertEqual(report["firing"], ["not-serving"])
        self.assertEqual(report["budgetFile"], {"version": self.budgets.version, "sha256": self.budgets.sha256})
        self.assertEqual(report["commit"], SHA)


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def run_cli(self, health_text: str | None, *extra: str) -> subprocess.CompletedProcess[str]:
        path = self.root / "health.json"
        if health_text is not None:
            path.write_text(health_text, encoding="utf-8")
        return subprocess.run([sys.executable, "-B", str(TOOL_DIR / "evaluate_server_alerts.py"), str(path), *extra],
                              capture_output=True, text=True, timeout=60)

    def test_clear_snapshot_exits_zero(self) -> None:
        run = self.run_cli(json.dumps(snapshot()))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout)["firing"], [])

    def test_firing_snapshot_exits_two_and_names_the_alert(self) -> None:
        run = self.run_cli(json.dumps(snapshot(live=False, ready=False)))
        self.assertEqual(run.returncode, 2, run.stderr)
        self.assertEqual(json.loads(run.stdout)["firing"], ["server-down"])
        self.assertIn("FIRING: server-down", run.stderr)

    def test_contract_violation_and_missing_file_exit_one(self) -> None:
        record = snapshot()
        del record["netQueueOut"]
        self.assertEqual(self.run_cli(json.dumps(record)).returncode, 1)
        self.assertEqual(self.run_cli(None).returncode, 1)

    def test_missing_budget_file_exits_one(self) -> None:
        run = self.run_cli(json.dumps(snapshot()), "--budgets", str(self.root / "absent.json"))
        self.assertEqual(run.returncode, 1)
        self.assertIn("budget file", run.stderr)


if __name__ == "__main__":
    unittest.main()
