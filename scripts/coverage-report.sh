#!/usr/bin/env bash
# coverage-report.sh — Per-subsystem coverage analysis from lcov data
#
# Usage: scripts/coverage-report.sh <coverage.info> [--json coverage.json]
#
# Parses lcov coverage.info and reports per-subsystem line coverage.
# Optionally outputs JSON for CI consumption.

set -euo pipefail

COVERAGE_FILE="${1:-coverage.info}"
JSON_OUTPUT=""
shift || true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --json)
            JSON_OUTPUT="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

if [[ ! -f "$COVERAGE_FILE" ]]; then
    echo "Error: Coverage file not found: $COVERAGE_FILE" >&2
    exit 1
fi

# Define subsystems and their source paths
declare -A SUBSYSTEMS=(
    ["Core"]="SparkEngine/Source/Core/"
    ["Graphics"]="SparkEngine/Source/Graphics/"
    ["Utils"]="SparkEngine/Source/Utils/"
    ["Camera"]="SparkEngine/Source/Camera/"
    ["ECS"]="SparkEngine/Source/Engine/ECS/"
    ["AI"]="SparkEngine/Source/Engine/AI/"
    ["Animation"]="SparkEngine/Source/Engine/Animation/"
    ["Networking"]="SparkEngine/Source/Engine/Networking/"
    ["Physics"]="SparkEngine/Source/Engine/Physics/"
    ["Audio"]="SparkEngine/Source/Engine/Audio/"
    ["Scripting"]="SparkEngine/Source/Engine/Scripting/"
    ["Editor"]="SparkEditor/Source/"
    ["GameModules"]="GameModules/"
)

# Define per-subsystem thresholds (minimum acceptable line coverage %)
declare -A THRESHOLDS=(
    ["Core"]=40
    ["Graphics"]=30
    ["Utils"]=60
    ["Camera"]=40
    ["ECS"]=40
    ["AI"]=35
    ["Animation"]=35
    ["Networking"]=35
    ["Physics"]=35
    ["Audio"]=30
    ["Scripting"]=30
    ["Editor"]=25
    ["GameModules"]=30
)

echo "=============================="
echo "Per-Subsystem Coverage Report"
echo "=============================="
printf "%-15s %8s %8s %8s %6s\n" "Subsystem" "Lines" "Hit" "Missed" "Cov%"
printf "%-15s %8s %8s %8s %6s\n" "---------" "-----" "---" "------" "----"

total_lines=0
total_hit=0
all_pass=true
json_entries=""

for subsystem in $(echo "${!SUBSYSTEMS[@]}" | tr ' ' '\n' | sort); do
    path="${SUBSYSTEMS[$subsystem]}"
    threshold="${THRESHOLDS[$subsystem]:-0}"

    # Extract coverage data for this subsystem from lcov info
    lines=0
    hit=0

    while IFS= read -r line; do
        case "$line" in
            SF:*"$path"*)
                in_subsystem=true
                ;;
            SF:*)
                in_subsystem=false
                ;;
            DA:*)
                if [[ "${in_subsystem:-false}" == "true" ]]; then
                    count="${line##*,}"
                    ((lines++)) || true
                    if [[ "$count" -gt 0 ]] 2>/dev/null; then
                        ((hit++)) || true
                    fi
                fi
                ;;
        esac
    done < "$COVERAGE_FILE"

    missed=$((lines - hit))
    if [[ $lines -gt 0 ]]; then
        pct=$(awk "BEGIN { printf \"%.1f\", ($hit / $lines) * 100 }")
    else
        pct="0.0"
    fi

    # Check threshold
    pass="true"
    icon="OK"
    if [[ $lines -gt 0 ]]; then
        below=$(awk "BEGIN { print ($pct < $threshold) ? 1 : 0 }")
        if [[ "$below" == "1" ]]; then
            pass="false"
            icon="FAIL"
            all_pass=false
        fi
    elif [[ $threshold -gt 0 ]]; then
        # A configured subsystem with no instrumented lines is missing
        # evidence, not 0% coverage that may silently pass.
        pass="false"
        icon="MISSING"
        all_pass=false
    fi

    printf "%-15s %8d %8d %8d %5s%% [%s >= %d%%]\n" \
        "$subsystem" "$lines" "$hit" "$missed" "$pct" "$icon" "$threshold"

    total_lines=$((total_lines + lines))
    total_hit=$((total_hit + hit))

    if [[ -n "$json_entries" ]]; then
        json_entries+=","
    fi
    json_entries+=$(printf '{"subsystem":"%s","lines":%d,"hit":%d,"coverage":%s,"threshold":%d,"pass":%s}' \
        "$subsystem" "$lines" "$hit" "$pct" "$threshold" "$pass")
done

echo "---------------------------------------------"
total_missed=$((total_lines - total_hit))
if [[ $total_lines -gt 0 ]]; then
    total_pct=$(awk "BEGIN { printf \"%.1f\", ($total_hit / $total_lines) * 100 }")
else
    total_pct="0.0"
fi
printf "%-15s %8d %8d %8d %5s%%\n" "SELECTED" "$total_lines" "$total_hit" "$total_missed" "$total_pct"

# The threshold table is intentionally curated. Report how much of the LCOV
# corpus falls outside it so selected-subsystem coverage is never presented as
# whole-repository coverage.
lcov_lines=0
lcov_hit=0
while IFS= read -r line; do
    case "$line" in
        DA:*)
            count="${line##*,}"
            ((lcov_lines++)) || true
            if [[ "$count" -gt 0 ]] 2>/dev/null; then
                ((lcov_hit++)) || true
            fi
            ;;
    esac
done < "$COVERAGE_FILE"
lcov_missed=$((lcov_lines - lcov_hit))
unclassified_lines=$((lcov_lines - total_lines))
if [[ $lcov_lines -gt 0 ]]; then
    lcov_pct=$(awk "BEGIN { printf \"%.1f\", ($lcov_hit / $lcov_lines) * 100 }")
else
    lcov_pct="0.0"
fi
printf "%-15s %8d %8d %8d %5s%%\n" "LCOV CORPUS" "$lcov_lines" "$lcov_hit" "$lcov_missed" "$lcov_pct"
printf "%-15s %8d\n" "UNCLASSIFIED" "$unclassified_lines"

# Output JSON if requested
if [[ -n "$JSON_OUTPUT" ]]; then
    cat > "$JSON_OUTPUT" <<JSONEOF
{
  "selected_subsystem_lines": $total_lines,
  "selected_subsystem_hit": $total_hit,
  "selected_subsystem_coverage": $total_pct,
  "lcov_corpus_lines": $lcov_lines,
  "lcov_corpus_hit": $lcov_hit,
  "lcov_corpus_coverage": $lcov_pct,
  "unclassified_lines": $unclassified_lines,
  "all_pass": $all_pass,
  "subsystems": [$json_entries]
}
JSONEOF
    echo ""
    echo "JSON report written to: $JSON_OUTPUT"
fi

if [[ "$all_pass" == "false" ]]; then
    echo ""
    echo "WARNING: Some subsystems are below their coverage threshold."
    exit 1
fi
