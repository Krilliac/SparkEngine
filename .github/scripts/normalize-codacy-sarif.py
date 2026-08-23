#!/usr/bin/env python3
"""Assign stable, unique categories to Codacy's per-language SARIF runs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
from typing import Any, Iterable
from urllib.parse import unquote, urlsplit


C_SOURCE_SUFFIXES = {".c", ".i"}
CPP_SOURCE_SUFFIXES = {".cc", ".cp", ".cpp", ".cppm", ".cxx", ".ii", ".ixx"}


def _artifact_uris(run: dict[str, Any]) -> Iterable[str]:
    for artifact in run.get("artifacts", []):
        if isinstance(artifact, dict):
            location = artifact.get("location", {})
            if isinstance(location, dict) and isinstance(location.get("uri"), str):
                yield location["uri"]

    for result in run.get("results", []):
        if not isinstance(result, dict):
            continue
        for location in result.get("locations", []):
            if not isinstance(location, dict):
                continue
            physical = location.get("physicalLocation", {})
            artifact = physical.get("artifactLocation", {}) if isinstance(physical, dict) else {}
            if isinstance(artifact, dict) and isinstance(artifact.get("uri"), str):
                yield artifact["uri"]


def _language_label(uris: set[str]) -> str:
    suffixes = {
        PurePosixPath(unquote(urlsplit(uri).path).replace("\\", "/")).suffix.lower()
        for uri in uris
    }
    if suffixes & CPP_SOURCE_SUFFIXES:
        return "cpp"
    if suffixes & C_SOURCE_SUFFIXES:
        return "c"
    return "generic"


def _tool_slug(run: dict[str, Any]) -> str:
    tool = run.get("tool", {})
    driver = tool.get("driver", {}) if isinstance(tool, dict) else {}
    name = driver.get("name", "codacy") if isinstance(driver, dict) else "codacy"
    slug = re.sub(r"[^a-z0-9]+", "-", str(name).lower()).strip("-")
    return slug or "codacy"


def normalize(payload: dict[str, Any]) -> list[str]:
    runs = payload.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("Codacy SARIF must contain at least one run")
    if not all(isinstance(run, dict) for run in runs):
        raise ValueError("Codacy SARIF runs must be objects")

    descriptors: list[tuple[dict[str, Any], str, str]] = []
    for run in runs:
        uris = set(_artifact_uris(run))
        label = f"{_tool_slug(run)}-{_language_label(uris)}"
        signature = hashlib.sha256("\n".join(sorted(uris)).encode("utf-8")).hexdigest()
        descriptors.append((run, label, signature))

    grouped: dict[str, list[tuple[dict[str, Any], str]]] = {}
    for run, label, signature in descriptors:
        grouped.setdefault(label, []).append((run, signature))

    assigned: list[str] = []
    for label in sorted(grouped):
        members = sorted(grouped[label], key=lambda item: item[1])
        for index, (run, _) in enumerate(members, start=1):
            suffix = "" if len(members) == 1 else f"-{index}"
            automation_id = f"codacy/{label}{suffix}/"
            details = run.setdefault("automationDetails", {})
            if not isinstance(details, dict):
                raise ValueError("run.automationDetails must be an object")
            details["id"] = automation_id
            assigned.append(automation_id)

    if len(assigned) != len(set(assigned)):
        raise ValueError("Codacy SARIF categories are not unique")
    return sorted(assigned)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sarif", type=Path)
    args = parser.parse_args()

    path = args.sarif.resolve()
    payload = json.loads(path.read_text(encoding="utf-8"))
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
    print(f"Assigned {len(assigned)} Codacy SARIF categories: {', '.join(assigned)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
