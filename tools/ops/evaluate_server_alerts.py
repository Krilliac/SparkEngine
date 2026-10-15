#!/usr/bin/env python3
"""OPS-110: evaluate the runbook alerts that one spark-server-health/1 snapshot can decide.

Reads a health snapshot (parsed and validated by tools/ops/validate_server_health.py)
and the versioned budget file tools/ops/server-soak-budgets.json, and reports
every alert as firing or clear:

* ``server-down`` -- ``live=false``: the server published its final snapshot and
  is no longer running.
* ``not-serving`` -- ``live`` and not ``ready`` and not ``draining``: running but
  not accepting work (a draining server is a deliberate stop, not an alert).
* ``tick-p95-over-budget`` / ``tick-p99-over-budget`` -- ``tickP95Us`` /
  ``tickP99Us`` above the ``tickP95Us`` / ``tickP99Us`` budgets, once
  ``tickSamples`` shows the percentiles cover at least one tick.
* ``queue-peak-at-cap`` -- ``netQueueInPeak`` or ``netQueueOutPeak`` at or above
  ``netQueueCap``: Unreliable traffic was already being shed.

Alerts that need a time series (a stale snapshot, RSS growth, queue-depth growth)
cannot be decided from one snapshot. Staleness is the recovery drill's job
(tools/ops/server_recovery_drill.py) and the two growth alerts are the soak
harness's (tools/ops/server_soak.py, same budget file).

CLI: ``evaluate_server_alerts.py <health.json> [--budgets <file>]`` prints a
``spark-server-alerts/1`` JSON document. Exit status 0: evaluated, nothing
firing. 2: evaluated, at least one alert firing. 1: the snapshot or the budget
file could not be read or violates its contract.

The budgets are provisional harness guards, not SLOs; see the runbook.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))

from server_soak import SoakBudgets, SoakError, load_budgets  # noqa: E402 -- sibling tool, the budget file
from validate_server_health import MAX_HEALTH_BYTES, HealthContractError, parse_health  # noqa: E402

ALERTS_SCHEMA = "spark-server-alerts/1"
EXIT_CLEAR = 0
EXIT_ERROR = 1
EXIT_FIRING = 2


@dataclass(frozen=True)
class Alert:
    name: str
    severity: str
    firing: bool
    detail: str

    def as_json(self) -> dict[str, Any]:
        return {"name": self.name, "severity": self.severity, "firing": self.firing, "detail": self.detail}


def evaluate_alerts(health: dict[str, Any], budgets: SoakBudgets) -> list[Alert]:
    """Every snapshot-decidable alert for a validated health snapshot, in a fixed order."""
    live, ready, draining = health["live"], health["ready"], health["draining"]
    alerts = [
        Alert("server-down", "page", not live,
              "live=false: the server has stopped" if not live else "live=true"),
        Alert("not-serving", "page", live and not ready and not draining,
              f"live={live}, ready={ready}, draining={draining}"),
    ]
    covers_ticks = health["tickSamples"] > 0
    for name, key, budget_key in (("tick-p95-over-budget", "tickP95Us", "tickP95Us"),
                                  ("tick-p99-over-budget", "tickP99Us", "tickP99Us")):
        ceiling = budgets.values[budget_key]
        firing = covers_ticks and health[key] > ceiling
        alerts.append(Alert(name, "warn", firing, f"{key} {health[key]} us against the budget {ceiling:g} us"
                            + ("" if covers_ticks else " (no ticks sampled yet)")))
    cap = budgets.values["netQueueCap"]
    peaks = {key: health[key] for key in ("netQueueInPeak", "netQueueOutPeak")}
    reached = sorted(key for key, peak in peaks.items() if peak >= cap)
    alerts.append(Alert("queue-peak-at-cap", "warn", bool(reached),
                        f"{', '.join(reached)} reached the cap {cap:g}" if reached
                        else f"peaks {peaks['netQueueInPeak']}/{peaks['netQueueOutPeak']} below the cap {cap:g}"))
    return alerts


def firing_names(alerts: list[Alert]) -> list[str]:
    return [alert.name for alert in alerts if alert.firing]


def build_report(health: dict[str, Any], budgets: SoakBudgets, source: str) -> dict[str, Any]:
    alerts = evaluate_alerts(health, budgets)
    return {"schema": ALERTS_SCHEMA, "source": source, "commit": health["commit"],
            "budgetFile": {"version": budgets.version, "sha256": budgets.sha256},
            "alerts": [alert.as_json() for alert in alerts], "firing": firing_names(alerts)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("health", type=Path, help="health snapshot file (SparkServer --health-file)")
    parser.add_argument("--budgets", type=Path, help="budget file (default: tools/ops/server-soak-budgets.json)")
    args = parser.parse_args(argv)
    try:
        budgets = load_budgets(args.budgets)
        with args.health.open("rb") as stream:
            data = stream.read(MAX_HEALTH_BYTES + 1)
        health = parse_health(data, source=str(args.health))
    except OSError as exc:
        print(f"evaluate_server_alerts: FAIL: cannot read {args.health}: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except (HealthContractError, SoakError) as exc:
        print(f"evaluate_server_alerts: FAIL: {exc}", file=sys.stderr)
        return EXIT_ERROR
    report = build_report(health, budgets, str(args.health))
    print(json.dumps(report, allow_nan=False))
    if report["firing"]:
        print(f"evaluate_server_alerts: FIRING: {', '.join(report['firing'])}", file=sys.stderr)
        return EXIT_FIRING
    print("evaluate_server_alerts: CLEAR: no alert firing", file=sys.stderr)
    return EXIT_CLEAR


if __name__ == "__main__":
    sys.exit(main())
