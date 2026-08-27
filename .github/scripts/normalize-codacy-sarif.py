#!/usr/bin/env python3
"""Normalize and deterministically shard Codacy SARIF for GitHub ingestion."""

from __future__ import annotations

import argparse
import copy
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
MAX_RESULTS_PER_RUN = 25_000
MAX_RUNS_PER_UPLOAD = 20


def _result_sort_key(result: Any) -> str:
    return json.dumps(result, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def split_oversized_runs(payload: dict[str, Any], max_results: int = MAX_RESULTS_PER_RUN) -> tuple[int, int]:
    if max_results < 1:
        raise ValueError("max_results must be positive")
    runs = payload.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("Codacy SARIF must contain at least one run")

    input_results = 0
    normalized_runs: list[dict[str, Any]] = []
    for run in runs:
        if not isinstance(run, dict):
            raise ValueError("Codacy SARIF runs must be objects")
        results = run.get("results", [])
        if not isinstance(results, list):
            raise ValueError("run.results must be an array")
        input_results += len(results)
        ordered = sorted(results, key=_result_sort_key)
        if not ordered:
            clone = copy.deepcopy(run)
            clone["results"] = []
            normalized_runs.append(clone)
            continue
        for offset in range(0, len(ordered), max_results):
            clone = copy.deepcopy(run)
            clone["results"] = ordered[offset : offset + max_results]
            normalized_runs.append(clone)

    if len(normalized_runs) > MAX_RUNS_PER_UPLOAD:
        raise ValueError(
            f"Codacy SARIF requires {len(normalized_runs)} runs, exceeding GitHub's "
            f"{MAX_RUNS_PER_UPLOAD}-run upload limit"
        )
    payload["runs"] = normalized_runs
    return input_results, len(normalized_runs)


def _level_counts(runs: list[dict[str, Any]]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for run in runs:
        for result in run.get("results", []):
            level = str(result.get("level", "none")) if isinstance(result, dict) else "invalid"
            counts[level] = counts.get(level, 0) + 1
    return dict(sorted(counts.items()))


def _results_digest(runs: list[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    canonical = sorted(_result_sort_key(result) for run in runs for result in run.get("results", []))
    for result in canonical:
        encoded = result.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    return digest.hexdigest()


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

    raw_runs = payload.get("runs")
    if not isinstance(raw_runs, list) or not raw_runs or not all(isinstance(run, dict) for run in raw_runs):
        raise ValueError("Codacy SARIF must contain object runs")
    input_runs = len(raw_runs)
    input_levels = _level_counts(raw_runs)
    input_digest = _results_digest(raw_runs)
    input_results, output_runs = split_oversized_runs(payload)
    assigned = normalize(payload)
    output_results = sum(len(run.get("results", [])) for run in payload["runs"])
    output_levels = _level_counts(payload["runs"])
    if output_results != input_results or output_levels != input_levels or _results_digest(payload["runs"]) != input_digest:
        raise ValueError("Codacy SARIF sharding changed the result multiset")
    if any(len(run.get("results", [])) > MAX_RESULTS_PER_RUN for run in payload["runs"]):
        raise ValueError("Codacy SARIF sharding left an oversized run")
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as stream:
        json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write("\n")
        temporary = Path(stream.name)
    os.replace(temporary, path)
    level_summary = ",".join(f"{level}:{count}" for level, count in input_levels.items()) or "none:0"
    summary = (
        f"Codacy SARIF: input_results={input_results} output_results={output_results} dropped_results=0 "
        f"input_runs={input_runs} output_runs={output_runs} max_results_per_run={MAX_RESULTS_PER_RUN} "
        f"levels={level_summary}; categories={','.join(assigned)}"
    )
    print(summary)
    if summary_path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(summary_path).open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(f"### Codacy SARIF normalization\n\n`{summary}`\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
