#!/usr/bin/env python3
"""Assign stable, unique categories to every MSVC analysis SARIF run."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
from typing import Any, Iterable


def _artifact_uris(run: dict[str, Any]) -> Iterable[str]:
    for artifact in run.get("artifacts", []):
        if isinstance(artifact, dict):
            location = artifact.get("location", {})
            if isinstance(location, dict) and isinstance(location.get("uri"), str):
                yield location["uri"].replace("\\", "/")

    for result in run.get("results", []):
        if not isinstance(result, dict):
            continue
        for location in result.get("locations", []):
            if not isinstance(location, dict):
                continue
            physical = location.get("physicalLocation", {})
            artifact = physical.get("artifactLocation", {}) if isinstance(physical, dict) else {}
            if isinstance(artifact, dict) and isinstance(artifact.get("uri"), str):
                yield artifact["uri"].replace("\\", "/")


def _slug(value: object, fallback: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", str(value).lower()).strip("-")
    return slug or fallback


def _run_label(run: dict[str, Any]) -> str:
    tool = run.get("tool", {})
    driver = tool.get("driver", {}) if isinstance(tool, dict) else {}
    name = driver.get("name", "msvc") if isinstance(driver, dict) else "msvc"
    return _slug(name, "msvc")


def _stable_signature(run: dict[str, Any]) -> str:
    """Describe a logical run without depending on its changing findings."""
    tool = run.get("tool", {})
    driver = tool.get("driver", {}) if isinstance(tool, dict) else {}
    automation = run.get("automationDetails", {})
    bases = run.get("originalUriBaseIds", {})
    descriptor = {
        "tool": {
            key: driver.get(key)
            for key in ("name", "fullName", "organization", "informationUri")
            if isinstance(driver, dict) and driver.get(key) is not None
        },
        "automation": {
            key: automation.get(key)
            for key in ("id", "guid", "correlationGuid", "description")
            if isinstance(automation, dict) and automation.get(key) is not None
        },
        "bases": bases if isinstance(bases, dict) else {},
        "artifacts": sorted(set(_artifact_uris(run))),
    }
    encoded = json.dumps(descriptor, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def normalize(payload: dict[str, Any]) -> list[str]:
    runs = payload.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("MSVC SARIF must contain at least one run")
    if not all(isinstance(run, dict) for run in runs):
        raise ValueError("MSVC SARIF runs must be objects")

    grouped: dict[str, list[tuple[str, int, dict[str, Any]]]] = {}
    for index, run in enumerate(runs):
        grouped.setdefault(_run_label(run), []).append((_stable_signature(run), index, run))

    assigned: list[str] = []
    for label in sorted(grouped):
        members = sorted(grouped[label], key=lambda item: (item[0], item[1]))
        for index, (_, _, run) in enumerate(members, start=1):
            suffix = "" if len(members) == 1 else f"-{index}"
            automation_id = f"msvc/{label}{suffix}/"
            details = run.setdefault("automationDetails", {})
            if not isinstance(details, dict):
                raise ValueError("run.automationDetails must be an object")
            details["id"] = automation_id
            assigned.append(automation_id)

    if len(assigned) != len(runs) or len(assigned) != len(set(assigned)):
        raise ValueError("MSVC SARIF categories are incomplete or not unique")
    return sorted(assigned)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sarif", type=Path)
    args = parser.parse_args()

    path = args.sarif.resolve()
    payload = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(payload, dict) or payload.get("version") != "2.1.0":
        raise ValueError("expected a SARIF 2.1.0 object")

    assigned = normalize(payload)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as stream:
        json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write("\n")
        temporary = Path(stream.name)
    os.replace(temporary, path)
    print(f"Assigned {len(assigned)} MSVC SARIF categories: {', '.join(assigned)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
