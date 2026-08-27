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
