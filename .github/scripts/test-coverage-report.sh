#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

if python3 -c 'import sys' >/dev/null 2>&1; then
    PYTHON=(python3)
elif python -c 'import sys' >/dev/null 2>&1; then
    PYTHON=(python)
elif py -3 -c 'import sys' >/dev/null 2>&1; then
    PYTHON=(py -3)
else
    echo "Python 3 is required" >&2
    exit 1
fi

cat > "$TMP_ROOT/coverage.info" <<'EOF'
TN:
SF:/repo/SparkEngine/Source/Core/Core.cpp
DA:1,1
end_of_record
SF:/repo/SparkEngine/Source/Audio/AudioMixer.cpp
DA:1,1
DA:2,0
end_of_record
SF:/repo/SparkEngine/Source/Physics/PhysicsSystem.cpp
DA:1,1
DA:2,1
DA:3,0
end_of_record
SF:/repo/SparkEngine/Source/Unclassified/Other.cpp
DA:1,0
end_of_record
EOF

set +e
bash "$REPO_ROOT/scripts/coverage-report.sh" "$TMP_ROOT/coverage.info" \
    --json "$TMP_ROOT/coverage.json" > "$TMP_ROOT/output.txt" 2>&1
status=$?
set -e

if [[ "$status" -eq 0 ]]; then
    echo "coverage report accepted missing mandatory subsystem evidence" >&2
    exit 1
fi

"${PYTHON[@]}" - "$TMP_ROOT/coverage.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    report = json.load(stream)
assert report["all_pass"] is False
assert report["selected_subsystem_lines"] == 6
assert report["selected_subsystem_hit"] == 4
assert report["lcov_corpus_lines"] == 7
assert report["lcov_corpus_hit"] == 4
assert report["unclassified_lines"] == 1
assert "total_coverage" not in report

subsystems = {entry["subsystem"]: entry for entry in report["subsystems"]}
assert subsystems["Audio"]["lines"] == 2
assert subsystems["Audio"]["hit"] == 1
assert subsystems["Audio"]["coverage"] == 50.0
assert subsystems["Physics"]["lines"] == 3
assert subsystems["Physics"]["hit"] == 2
assert subsystems["Physics"]["coverage"] == 66.7
PY

grep -Fq "MISSING" "$TMP_ROOT/output.txt"
grep -Fq "SELECTED" "$TMP_ROOT/output.txt"
grep -Fq "LCOV CORPUS" "$TMP_ROOT/output.txt"
grep -Fq "UNCLASSIFIED" "$TMP_ROOT/output.txt"

# The workflow must reject a non-empty but partially generated LCOV corpus.
# scripts/coverage-report.sh cannot reconstruct capture/remove/list failures
# from the trace file alone, so keep that provenance gate at the producer.
WORKFLOW="$REPO_ROOT/.github/workflows/build.yml"
grep -Fq 'if [ "$cap_exit" -ne 0 ] || [ "$rm_exit" -ne 0 ] || [ "$list_exit" -ne 0 ]; then' "$WORKFLOW"
grep -Fq 'lcov coverage evidence is incomplete' "$WORKFLOW"

# PR-controlled build code must never share a job with pull-request write
# permission. Coverage crosses that boundary through one small, same-run data
# artifact into a reporter that does not check out or execute repository code.
"${PYTHON[@]}" - "$WORKFLOW" <<'PY'
import re
import sys
from pathlib import Path

workflow = Path(sys.argv[1]).read_text(encoding="utf-8")


def job_block(name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
        workflow,
    )
    assert match, f"missing workflow job: {name}"
    return match.group(1)


top_permissions = re.search(r"(?ms)^permissions:\n((?:  [^\n]*\n)+)", workflow)
assert top_permissions, "missing workflow-wide permissions"
assert "contents: read" in top_permissions.group(1)
assert "pull-requests: write" not in top_permissions.group(1)

coverage_job = job_block("coverage")
assert re.search(r"(?m)^    permissions:\n      contents: read$", coverage_job)
assert "pull-requests: write" not in coverage_job
assert "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1" in coverage_job
assert "Upload bounded coverage comment payload" in coverage_job
assert "coverage-pr-comment-${{ github.run_id }}-${{ github.run_attempt }}-${{ github.sha }}" in coverage_job
assert "path: coverage-pr-comment-payload/payload.json" in coverage_job
assert "retention-days: 1" in coverage_job

reporter_job = job_block("report-coverage")
assert "needs: coverage" in reporter_job
assert re.search(r"(?m)^    permissions:\n      actions: read\n      pull-requests: write$", reporter_job)
assert "actions/checkout@" not in reporter_job
assert not re.search(r"(?m)^      - .*\n(?:        .*\n)*?        run:", reporter_job)
assert "actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c" in reporter_job
assert "actions/github-script@3a2844b7e9c422d3c10d287c895573f7108da1b3" in reporter_job
assert "listWorkflowRunArtifacts" in reporter_job
for field in (
    "artifact_name",
    "source_event",
    "source_run_id",
    "source_run_attempt",
    "source_sha",
    "source_head_sha",
    "source_repository",
    "pull_request_number",
):
    assert f"payload[field]" in reporter_job or field in reporter_job
assert "payloadStat.size > 131072" in reporter_job
assert "payload.body.length > 60000" in reporter_job

ci_error_reporter = job_block("report-ci-errors")
assert "pull-requests: write" in ci_error_reporter
assert "ref: Working" in ci_error_reporter
assert "persist-credentials: false" in ci_error_reporter

assert workflow.count("pull-requests: write") == 2
PY
