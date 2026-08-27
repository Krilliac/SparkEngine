#!/usr/bin/env python3
"""Validate a direct Codacy SARIF artifact before privileged upload."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
from typing import Any


ARTIFACT_NAME = "results.sarif"
MAX_SARIF_BYTES = 100 * 1024 * 1024
MAX_RESULTS_PER_RUN = 25_000
MAX_TOTAL_RESULTS = 75_000
EXPECTED_TOOL_NAME = "Cppcheck (reported by Codacy)"
EXPECTED_CATEGORIES = (
    "codacy/cppcheck-reported-by-codacy-c/",
    "codacy/cppcheck-reported-by-codacy-cpp-1/",
    "codacy/cppcheck-reported-by-codacy-cpp-2/",
)
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$", re.IGNORECASE)
DIGEST_PATTERN = re.compile(r"^sha256:([0-9a-f]{64})$", re.IGNORECASE)


def _object_without_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON property: {key}")
        value[key] = item
    return value


def _required_environment(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise ValueError(f"{name} is required")
    return value


def _validate_upload_provenance() -> None:
    event = _required_environment("EXPECTED_SOURCE_EVENT")
    source_sha = _required_environment("EXPECTED_SOURCE_SHA").lower()
    upload_ref = _required_environment("EXPECTED_UPLOAD_REF")
    upload_sha = _required_environment("EXPECTED_UPLOAD_SHA").lower()
    default_branch = _required_environment("EXPECTED_DEFAULT_BRANCH")
    pull_number = os.environ.get("EXPECTED_PR_NUMBER", "")

    if not SHA_PATTERN.fullmatch(source_sha) or not SHA_PATTERN.fullmatch(upload_sha):
        raise ValueError("source and upload SHAs must be exact 40-hex commit IDs")
    if event == "pull_request":
        if not pull_number.isdecimal() or int(pull_number) < 1:
            raise ValueError("pull_request upload requires an exact positive PR number")
        if upload_ref != f"refs/pull/{int(pull_number)}/head":
            raise ValueError("pull_request upload ref must be the exact head ref")
        if upload_sha != source_sha:
            raise ValueError("pull_request upload SHA must equal the source-run head SHA")
    elif event in {"push", "schedule"}:
        if pull_number:
            raise ValueError("non-PR upload must not carry a pull-request number")
        if upload_ref != f"refs/heads/{default_branch}":
            raise ValueError("push/schedule upload ref must be the exact default branch")
        if upload_sha != source_sha:
            raise ValueError("push/schedule upload SHA must equal the source-run head SHA")
    else:
        raise ValueError(f"unsupported source event: {event!r}")


def _validate_result(result: Any) -> None:
    if not isinstance(result, dict):
        raise ValueError("SARIF results must be objects")
    if not isinstance(result.get("ruleId"), str) or not result["ruleId"]:
        raise ValueError("SARIF result.ruleId must be a non-empty string")
    if result.get("level") not in {"warning", "error"}:
        raise ValueError("normalized Codacy results must be warning or error severity")
    if result["ruleId"] == "cppcheck_misra-config":
        raise ValueError("suppressed cppcheck_misra-config result survived normalization")


def validate_artifact(directory: Path, expected_digest: str) -> dict[str, int]:
    directory_metadata = directory.lstat()
    if not stat.S_ISDIR(directory_metadata.st_mode) or directory.is_symlink():
        raise ValueError("artifact download path must be a real directory")

    entries = list(directory.iterdir())
    if len(entries) != 1 or entries[0].name != ARTIFACT_NAME:
        raise ValueError(f"artifact directory must contain only {ARTIFACT_NAME}")
    artifact = entries[0]
    metadata = artifact.lstat()
    if not stat.S_ISREG(metadata.st_mode) or artifact.is_symlink():
        raise ValueError(f"{ARTIFACT_NAME} must be a direct regular file")
    if metadata.st_size < 1 or metadata.st_size > MAX_SARIF_BYTES:
        raise ValueError(
            f"{ARTIFACT_NAME} size must be within 1..{MAX_SARIF_BYTES} bytes"
        )

    digest_match = DIGEST_PATTERN.fullmatch(expected_digest)
    if digest_match is None:
        raise ValueError("expected artifact digest must be an exact SHA-256 digest")
    digest = hashlib.sha256()
    with artifact.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    actual_digest = digest.hexdigest()
    if actual_digest.lower() != digest_match.group(1).lower():
        raise ValueError("downloaded direct file does not match the authorized artifact digest")

    with artifact.open("r", encoding="utf-8-sig") as stream:
        payload = json.load(stream, object_pairs_hook=_object_without_duplicate_keys)
    if not isinstance(payload, dict) or payload.get("version") != "2.1.0":
        raise ValueError("artifact must be a SARIF 2.1.0 object")
    runs = payload.get("runs")
    if not isinstance(runs, list) or len(runs) != len(EXPECTED_CATEGORIES):
        raise ValueError("artifact must contain the exact three-run Codacy category roster")

    total_results = 0
    for index, (run, expected_category) in enumerate(zip(runs, EXPECTED_CATEGORIES)):
        if not isinstance(run, dict):
            raise ValueError(f"SARIF run {index} must be an object")
        driver = run.get("tool", {}).get("driver") if isinstance(run.get("tool"), dict) else None
        if not isinstance(driver, dict) or driver.get("name") != EXPECTED_TOOL_NAME:
            raise ValueError(f"SARIF run {index} does not use the exact Codacy cppcheck tool")
        automation = run.get("automationDetails")
        if not isinstance(automation, dict) or automation.get("id") != expected_category:
            raise ValueError(f"SARIF run {index} has an unexpected automation category")
        results = run.get("results")
        if not isinstance(results, list) or len(results) > MAX_RESULTS_PER_RUN:
            raise ValueError(
                f"SARIF run {index} must contain at most {MAX_RESULTS_PER_RUN} results"
            )
        total_results += len(results)
        if total_results > MAX_TOTAL_RESULTS:
            raise ValueError("SARIF artifact exceeds the total result limit")
        for result in results:
            _validate_result(result)

    return {"bytes": metadata.st_size, "runs": len(runs), "results": total_results}


def main() -> int:
    _validate_upload_provenance()
    directory = Path(_required_environment("ARTIFACT_DIR"))
    summary = validate_artifact(directory, _required_environment("EXPECTED_ARTIFACT_DIGEST"))
    print(
        "Validated direct Codacy SARIF: "
        f"{summary['bytes']} bytes, {summary['runs']} runs, {summary['results']} results"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
