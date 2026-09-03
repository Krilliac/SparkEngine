#!/usr/bin/env python3
"""Filter, audit, and deterministically shard Codacy SARIF for GitHub ingestion."""

from __future__ import annotations

import argparse
from collections import Counter
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
VALID_RESULT_LEVELS = frozenset({"none", "note", "warning", "error"})
DROP_LEVELS = frozenset({"none", "note"})
# cppcheck_misra-config: configuration chatter, not a finding.
# cppcheck_y2038-unsafe-call: the y2038 addon warns about 32-bit time_t; every
# supported SparkEngine target (Windows x64, Linux x86_64, macOS) has a 64-bit
# time_t, so these calls are not unsafe here.
DROP_RULE_IDS = frozenset({"cppcheck_misra-config", "cppcheck_y2038-unsafe-call"})
# Codacy's cppcheck runner parses plain .h headers as C. A C++ header therefore
# fails at its first namespace or class with "Code '...' is invalid C code",
# which describes the scanner's language guess, not the source.
HEADER_PARSED_AS_C_RULE_ID = "cppcheck_syntaxError"
HEADER_PARSED_AS_C_SUFFIX = ".h"
HEADER_PARSED_AS_C_MESSAGE = re.compile(r"\bis invalid C code\b")
EXPECTED_TOOL_SLUG = "cppcheck-reported-by-codacy"
CATEGORY_ROSTER = (
    "codacy/cppcheck-reported-by-codacy-c/",
    "codacy/cppcheck-reported-by-codacy-cpp-1/",
    "codacy/cppcheck-reported-by-codacy-cpp-2/",
)
CATEGORY_SLOTS = {
    "c": CATEGORY_ROSTER[:1],
    "cpp": CATEGORY_ROSTER[1:],
}


def _result_sort_key(result: Any) -> str:
    return json.dumps(result, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _strings_digest(values: Iterable[str]) -> str:
    digest = hashlib.sha256()
    for value in sorted(values):
        encoded = value.encode("utf-8")
        digest.update(len(encoded).to_bytes(8, "big"))
        digest.update(encoded)
    return digest.hexdigest()


def _results_digest(results: Iterable[dict[str, Any]]) -> str:
    return _strings_digest(_result_sort_key(result) for result in results)


def _array(run: dict[str, Any], key: str) -> list[Any]:
    value = run.get(key, [])
    if not isinstance(value, list):
        raise ValueError(f"run.{key} must be an array")
    return value


def _validate_artifact_location(value: Any, context: str) -> None:
    if not isinstance(value, dict):
        raise ValueError(f"{context} must be an object")
    if "uri" in value and (not isinstance(value["uri"], str) or not value["uri"]):
        raise ValueError(f"{context}.uri must be a non-empty string")


def _validate_run(run: dict[str, Any]) -> None:
    tool = run.get("tool")
    if not isinstance(tool, dict):
        raise ValueError("run.tool must be an object")
    driver = tool.get("driver")
    if not isinstance(driver, dict) or not isinstance(driver.get("name"), str) or not driver["name"]:
        raise ValueError("run.tool.driver must have a non-empty string name")

    automation = run.get("automationDetails")
    if automation is not None and not isinstance(automation, dict):
        raise ValueError("run.automationDetails must be an object")

    for artifact in _array(run, "artifacts"):
        if not isinstance(artifact, dict):
            raise ValueError("run.artifacts entries must be objects")
        if "location" in artifact:
            _validate_artifact_location(artifact["location"], "artifact.location")

    for result in _array(run, "results"):
        if not isinstance(result, dict):
            raise ValueError("run.results entries must be objects")
        rule_id = result.get("ruleId")
        if not isinstance(rule_id, str) or not rule_id:
            raise ValueError("result.ruleId must be a non-empty string")
        level = result.get("level")
        if not isinstance(level, str) or level not in VALID_RESULT_LEVELS:
            allowed = ", ".join(sorted(VALID_RESULT_LEVELS))
            raise ValueError(f"result.level must be explicit and one of: {allowed}")
        locations = result.get("locations", [])
        if not isinstance(locations, list):
            raise ValueError("result.locations must be an array")
        for location in locations:
            if not isinstance(location, dict):
                raise ValueError("result.locations entries must be objects")
            if "physicalLocation" not in location:
                continue
            physical = location["physicalLocation"]
            if not isinstance(physical, dict):
                raise ValueError("location.physicalLocation must be an object")
            if "artifactLocation" in physical:
                _validate_artifact_location(
                    physical["artifactLocation"], "physicalLocation.artifactLocation"
                )


def _validate_payload(payload: Any) -> list[dict[str, Any]]:
    if not isinstance(payload, dict) or payload.get("version") != "2.1.0":
        raise ValueError("expected a SARIF 2.1.0 object")
    runs = payload.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValueError("Codacy SARIF must contain at least one run")
    if not all(isinstance(run, dict) for run in runs):
        raise ValueError("Codacy SARIF runs must be objects")
    for run in runs:
        _validate_run(run)
    return runs


def _all_results(runs: Iterable[dict[str, Any]]) -> Iterable[dict[str, Any]]:
    for run in runs:
        for result in _array(run, "results"):
            if not isinstance(result, dict):
                raise ValueError("run.results entries must be objects")
            yield result


def split_oversized_runs(
    payload: dict[str, Any], max_results: int = MAX_RESULTS_PER_RUN
) -> tuple[int, int]:
    if max_results < 1:
        raise ValueError("max_results must be positive")
    runs = _validate_payload(payload)

    input_results = 0
    normalized_runs: list[dict[str, Any]] = []
    for run in runs:
        results = _array(run, "results")
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


def _level_counts(results: Iterable[dict[str, Any]]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for result in results:
        level = result.get("level")
        if not isinstance(level, str) or level not in VALID_RESULT_LEVELS:
            raise ValueError("cannot audit a result with an invalid level")
        counts[level] += 1
    return dict(sorted(counts.items()))


def _artifact_uris(run: dict[str, Any]) -> Iterable[str]:
    for artifact in _array(run, "artifacts"):
        location = artifact.get("location")
        if isinstance(location, dict) and isinstance(location.get("uri"), str):
            yield location["uri"]

    for result in _array(run, "results"):
        for location in result.get("locations", []):
            physical = location.get("physicalLocation")
            artifact = physical.get("artifactLocation") if isinstance(physical, dict) else None
            if isinstance(artifact, dict) and isinstance(artifact.get("uri"), str):
                yield artifact["uri"]


def _language_label(uris: set[str]) -> str:
    suffixes = {
        PurePosixPath(unquote(urlsplit(uri).path).replace("\\", "/")).suffix.lower()
        for uri in uris
    }
    has_c = bool(suffixes & C_SOURCE_SUFFIXES)
    has_cpp = bool(suffixes & CPP_SOURCE_SUFFIXES)
    if has_c and has_cpp:
        raise ValueError("Codacy SARIF run mixes C and C++ source artifacts")
    if has_cpp:
        return "cpp"
    if has_c:
        return "c"
    raise ValueError("Codacy SARIF run has no recognizable C or C++ source artifacts")


def _tool_slug(run: dict[str, Any]) -> str:
    driver = run["tool"]["driver"]
    slug = re.sub(r"[^a-z0-9]+", "-", driver["name"].lower()).strip("-")
    return slug or "codacy"


def _run_signature(run: dict[str, Any]) -> tuple[str, str]:
    uris = set(_artifact_uris(run))
    uri_digest = hashlib.sha256("\n".join(sorted(uris)).encode("utf-8")).hexdigest()
    # The URI digest preserves the legacy category assignment. The full digest
    # only breaks otherwise ambiguous ties deterministically.
    full_digest = hashlib.sha256(_result_sort_key(run).encode("utf-8")).hexdigest()
    return uri_digest, full_digest


def _automation_id(run: dict[str, Any]) -> str | None:
    details = run.get("automationDetails")
    if details is None or "id" not in details:
        return None
    automation_id = details["id"]
    if not isinstance(automation_id, str) or not automation_id:
        raise ValueError("run.automationDetails.id must be a non-empty string")
    return automation_id


def _set_automation_id(run: dict[str, Any], automation_id: str) -> None:
    details = run.setdefault("automationDetails", {})
    if not isinstance(details, dict):
        raise ValueError("run.automationDetails must be an object")
    details["id"] = automation_id


def _empty_run(template: dict[str, Any], automation_id: str) -> dict[str, Any]:
    clone = copy.deepcopy(template)
    clone["results"] = []
    _set_automation_id(clone, automation_id)
    return clone


def _existing_roster(runs: list[dict[str, Any]]) -> list[dict[str, Any]] | None:
    automation_ids = [_automation_id(run) for run in runs]
    if not any(automation_ids):
        return None
    if any(automation_id is None for automation_id in automation_ids):
        raise ValueError("Codacy SARIF mixes categorized and uncategorized runs")
    if len(automation_ids) != len(CATEGORY_ROSTER) or set(automation_ids) != set(CATEGORY_ROSTER):
        raise ValueError("Codacy SARIF automationDetails category roster is unexpected")
    if len(set(automation_ids)) != len(automation_ids):
        raise ValueError("Codacy SARIF categories are not unique")
    by_id = {automation_id: run for automation_id, run in zip(automation_ids, runs)}
    return [by_id[automation_id] for automation_id in CATEGORY_ROSTER]


def normalize(payload: dict[str, Any]) -> list[str]:
    runs = _validate_payload(payload)
    existing = _existing_roster(runs)
    if existing is not None:
        payload["runs"] = existing
        return list(CATEGORY_ROSTER)

    grouped: dict[str, list[dict[str, Any]]] = {"c": [], "cpp": []}
    for run in runs:
        if _tool_slug(run) != EXPECTED_TOOL_SLUG:
            raise ValueError("Codacy SARIF tool name no longer matches the stable category roster")
        grouped[_language_label(set(_artifact_uris(run)))].append(run)

    for language, members in grouped.items():
        members.sort(key=_run_signature)
        capacity = len(CATEGORY_SLOTS[language])
        if len(members) > capacity:
            raise ValueError(
                f"Codacy SARIF needs {len(members)} {language} categories; stable roster has {capacity}"
            )

    template = runs[0]
    output_runs: list[dict[str, Any]] = []
    original_results = sum(len(_array(run, "results")) for run in runs)
    for language in ("c", "cpp"):
        members = grouped[language]
        for index, automation_id in enumerate(CATEGORY_SLOTS[language]):
            if index < len(members):
                run = members[index]
                _set_automation_id(run, automation_id)
            else:
                run = _empty_run(template, automation_id)
            output_runs.append(run)

    output_results = sum(len(_array(run, "results")) for run in output_runs)
    if output_results != original_results:
        raise ValueError("Codacy SARIF category migration changed the result count")
    payload["runs"] = output_runs
    return list(CATEGORY_ROSTER)


def _primary_uri(result: dict[str, Any]) -> str:
    for location in result.get("locations", []):
        physical = location.get("physicalLocation") if isinstance(location, dict) else None
        artifact = physical.get("artifactLocation") if isinstance(physical, dict) else None
        uri = artifact.get("uri") if isinstance(artifact, dict) else None
        if isinstance(uri, str):
            return uri
    return ""


def _is_header_parsed_as_c(result: dict[str, Any]) -> bool:
    if result["ruleId"] != HEADER_PARSED_AS_C_RULE_ID:
        return False
    message = result.get("message")
    text = message.get("text") if isinstance(message, dict) else None
    if not isinstance(text, str) or not HEADER_PARSED_AS_C_MESSAGE.search(text):
        return False
    uri = _primary_uri(result)
    return PurePosixPath(urlsplit(uri).path).suffix.lower() == HEADER_PARSED_AS_C_SUFFIX


def _drop_reason(result: dict[str, Any]) -> str | None:
    rule_id = result["ruleId"]
    level = result["level"]
    if rule_id in DROP_RULE_IDS:
        return f"rule:{rule_id}"
    if _is_header_parsed_as_c(result):
        return f"header-parsed-as-c:{rule_id}"
    if level in DROP_LEVELS:
        return f"level:{level}"
    return None


def filter_for_github(payload: dict[str, Any]) -> dict[str, Any]:
    runs = _validate_payload(payload)
    existing = _existing_roster(runs)
    if existing is None:
        raise ValueError("Codacy SARIF must be categorized before filtering")
    payload["runs"] = existing

    input_results = list(_all_results(existing))
    retained: list[dict[str, Any]] = []
    dropped: list[dict[str, Any]] = []
    dropped_reasons: Counter[str] = Counter()
    dropped_rules: Counter[str] = Counter()
    decision_records: list[str] = []
    category_counts: dict[str, dict[str, int]] = {}

    for run in existing:
        automation_id = _automation_id(run)
        if automation_id is None:
            raise ValueError("categorized run lost automationDetails.id")
        before = list(_array(run, "results"))
        kept_in_run: list[dict[str, Any]] = []
        for result in before:
            reason = _drop_reason(result)
            if reason is None:
                if result["level"] not in {"warning", "error"}:
                    raise ValueError("filter retained a non-actionable SARIF level")
                kept_in_run.append(result)
                retained.append(result)
                decision_records.append(f"keep\0{_result_sort_key(result)}")
            else:
                dropped.append(result)
                dropped_reasons[reason] += 1
                dropped_rules[result["ruleId"]] += 1
                decision_records.append(f"drop\0{reason}\0{_result_sort_key(result)}")
        run["results"] = kept_in_run
        category_counts[automation_id] = {
            "input": len(before),
            "retained": len(kept_in_run),
            "dropped": len(before) - len(kept_in_run),
        }

    input_multiset = Counter(_result_sort_key(result) for result in input_results)
    partitioned_multiset = Counter(_result_sort_key(result) for result in (*retained, *dropped))
    if input_multiset != partitioned_multiset:
        raise ValueError("Codacy SARIF filtering changed the result multiset")
    if any(
        result["level"] not in {"warning", "error"} or _drop_reason(result) is not None
        for result in retained
    ):
        raise ValueError("Codacy SARIF filtering retained a suppressed diagnostic")

    return {
        "input_results": len(input_results),
        "output_results": len(retained),
        "dropped_results": len(dropped),
        "input_levels": _level_counts(input_results),
        "output_levels": _level_counts(retained),
        "dropped_levels": _level_counts(dropped),
        "dropped_reasons": dict(sorted(dropped_reasons.items())),
        "dropped_rules": dict(sorted(dropped_rules.items())),
        "input_digest": _results_digest(input_results),
        "output_digest": _results_digest(retained),
        "dropped_digest": _results_digest(dropped),
        "decision_digest": _strings_digest(decision_records),
        "categories": list(CATEGORY_ROSTER),
        "category_counts": category_counts,
    }


def normalize_for_github(
    payload: dict[str, Any], max_results: int = MAX_RESULTS_PER_RUN
) -> dict[str, Any]:
    raw_runs = _validate_payload(payload)
    input_runs = len(raw_runs)
    input_results = list(_all_results(raw_runs))
    input_digest = _results_digest(input_results)
    input_levels = _level_counts(input_results)

    sharded_results, _ = split_oversized_runs(payload, max_results=max_results)
    assigned = normalize(payload)
    categorized_results = list(_all_results(payload["runs"]))
    if (
        len(categorized_results) != sharded_results
        or sharded_results != len(input_results)
        or _results_digest(categorized_results) != input_digest
        or _level_counts(categorized_results) != input_levels
    ):
        raise ValueError("Codacy SARIF sharding or category migration changed the result multiset")
    if any(len(_array(run, "results")) > max_results for run in payload["runs"]):
        raise ValueError("Codacy SARIF sharding left an oversized run")

    audit = filter_for_github(payload)
    if audit["input_results"] != len(input_results) or audit["input_digest"] != input_digest:
        raise ValueError("Codacy SARIF audit does not match the input result multiset")
    audit.update(
        {
            "input_runs": input_runs,
            "output_runs": len(payload["runs"]),
            "max_results_per_run": max_results,
            "categories": assigned,
        }
    )
    return audit


def _count_text(counts: dict[str, int]) -> str:
    return ",".join(f"{key}={value}" for key, value in sorted(counts.items())) or "none"


def _summary_line(audit: dict[str, Any]) -> str:
    return (
        f"Codacy SARIF: input_results={audit['input_results']} "
        f"output_results={audit['output_results']} dropped_results={audit['dropped_results']} "
        f"input_runs={audit['input_runs']} output_runs={audit['output_runs']} "
        f"max_results_per_run={audit['max_results_per_run']} "
        f"input_levels={_count_text(audit['input_levels'])}; "
        f"output_levels={_count_text(audit['output_levels'])}; "
        f"dropped_reasons={_count_text(audit['dropped_reasons'])}; "
        f"categories={','.join(audit['categories'])}"
    )


def _markdown_table(stream: Any, headers: tuple[str, ...], rows: Iterable[tuple[Any, ...]]) -> None:
    stream.write("| " + " | ".join(headers) + " |\n")
    stream.write("| " + " | ".join("---" for _ in headers) + " |\n")
    wrote_row = False
    for row in rows:
        stream.write("| " + " | ".join(str(value).replace("|", "\\|") for value in row) + " |\n")
        wrote_row = True
    if not wrote_row:
        stream.write("| _none_ | " + " | ".join("0" for _ in headers[1:]) + " |\n")


def _write_step_summary(path: Path, audit: dict[str, Any], summary: str) -> None:
    with path.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write("### Codacy SARIF normalization\n\n")
        stream.write(f"`{summary}`\n\n")
        stream.write(
            "Only explicit `warning`/`error` results are uploaded; `none`, `note`, and exact "
            "rule `cppcheck_misra-config` diagnostics are omitted.\n\n"
        )
        stream.write("#### Audit digests\n\n")
        _markdown_table(
            stream,
            ("Set", "SHA-256"),
            (
                ("input", audit["input_digest"]),
                ("retained", audit["output_digest"]),
                ("dropped", audit["dropped_digest"]),
                ("decisions", audit["decision_digest"]),
            ),
        )
        stream.write("\n#### Categories\n\n")
        _markdown_table(
            stream,
            ("automationDetails.id", "Input", "Retained", "Dropped"),
            (
                (
                    category,
                    audit["category_counts"][category]["input"],
                    audit["category_counts"][category]["retained"],
                    audit["category_counts"][category]["dropped"],
                )
                for category in audit["categories"]
            ),
        )
        stream.write("\n#### Levels\n\n")
        levels = sorted(
            set(audit["input_levels"])
            | set(audit["output_levels"])
            | set(audit["dropped_levels"])
        )
        _markdown_table(
            stream,
            ("Level", "Input", "Retained", "Dropped"),
            (
                (
                    level,
                    audit["input_levels"].get(level, 0),
                    audit["output_levels"].get(level, 0),
                    audit["dropped_levels"].get(level, 0),
                )
                for level in levels
            ),
        )
        stream.write("\n#### Dropped by reason\n\n")
        _markdown_table(
            stream,
            ("Reason", "Count"),
            ((reason, count) for reason, count in audit["dropped_reasons"].items()),
        )
        stream.write("\n#### Dropped by rule\n\n")
        _markdown_table(
            stream,
            ("Rule", "Count"),
            ((rule, count) for rule, count in audit["dropped_rules"].items()),
        )
        stream.write("\n")


def _object_without_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON property: {key}")
        value[key] = item
    return value


def _load_payload(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(
            path.read_text(encoding="utf-8-sig"), object_pairs_hook=_object_without_duplicate_keys
        )
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid SARIF JSON: {error.msg}") from error
    _validate_payload(payload)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sarif", type=Path)
    args = parser.parse_args()

    path = args.sarif.resolve()
    payload = _load_payload(path)
    audit = normalize_for_github(payload)
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", newline="\n", dir=path.parent, delete=False
    ) as stream:
        json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write("\n")
        temporary = Path(stream.name)
    os.replace(temporary, path)

    summary = _summary_line(audit)
    print(summary)
    print(
        f"Codacy SARIF digests: input={audit['input_digest']} retained={audit['output_digest']} "
        f"dropped={audit['dropped_digest']} decisions={audit['decision_digest']}"
    )
    print(f"Codacy SARIF dropped by reason: {_count_text(audit['dropped_reasons'])}")
    print(f"Codacy SARIF dropped by rule: {_count_text(audit['dropped_rules'])}")
    if summary_path := os.environ.get("GITHUB_STEP_SUMMARY"):
        _write_step_summary(Path(summary_path), audit, summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
