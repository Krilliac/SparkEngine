#!/usr/bin/env bash
# scripts/coverage-report.sh — Per-subsystem coverage analysis for SparkEngine
#
# Usage:
#   scripts/coverage-report.sh <coverage.info> [--check] [--json <out.json>]
#
# Reads an lcov coverage.info file and produces a per-subsystem breakdown.
# With --check, enforces minimum thresholds and exits non-zero on failure.
# With --json, writes machine-readable results for CI consumption.

set -euo pipefail

COVERAGE_FILE="${1:-coverage.info}"
CHECK_MODE=false
JSON_FILE=""

shift || true
while [[ $# -gt 0 ]]; do
    case "$1" in
        --check) CHECK_MODE=true; shift ;;
        --json)  JSON_FILE="$2"; shift 2 ;;
        *)       echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ ! -f "$COVERAGE_FILE" ]]; then
    echo "Error: Coverage file not found: $COVERAGE_FILE"
    exit 1
fi

# ============================================================================
# Subsystem definitions: name -> path pattern(s)
#
# Each subsystem maps to one or more directory prefixes in the coverage data.
# The thresholds are minimum acceptable line-coverage percentages.
# ============================================================================

declare -A SUBSYSTEM_PATTERNS
declare -A SUBSYSTEM_THRESHOLDS

# Engine core
SUBSYSTEM_PATTERNS[Core]="SparkEngine/Source/Core/"
SUBSYSTEM_THRESHOLDS[Core]=25

SUBSYSTEM_PATTERNS[Utils]="SparkEngine/Source/Utils/"
SUBSYSTEM_THRESHOLDS[Utils]=20

SUBSYSTEM_PATTERNS[Camera]="SparkEngine/Source/Camera/"
SUBSYSTEM_THRESHOLDS[Camera]=10

SUBSYSTEM_PATTERNS[Audio]="SparkEngine/Source/Audio/"
SUBSYSTEM_THRESHOLDS[Audio]=5

SUBSYSTEM_PATTERNS[Graphics]="SparkEngine/Source/Graphics/"
SUBSYSTEM_THRESHOLDS[Graphics]=5

SUBSYSTEM_PATTERNS[Input]="SparkEngine/Source/Input/"
SUBSYSTEM_THRESHOLDS[Input]=10

SUBSYSTEM_PATTERNS[Physics]="SparkEngine/Source/Physics/"
SUBSYSTEM_THRESHOLDS[Physics]=10

SUBSYSTEM_PATTERNS[SceneManager]="SparkEngine/Source/SceneManager/"
SUBSYSTEM_THRESHOLDS[SceneManager]=5

# Engine subsystems
SUBSYSTEM_PATTERNS[AI]="SparkEngine/Source/Engine/AI/"
SUBSYSTEM_THRESHOLDS[AI]=10

SUBSYSTEM_PATTERNS[Animation]="SparkEngine/Source/Engine/Animation/"
SUBSYSTEM_THRESHOLDS[Animation]=10

SUBSYSTEM_PATTERNS[ECS]="SparkEngine/Source/Engine/ECS/"
SUBSYSTEM_THRESHOLDS[ECS]=15

SUBSYSTEM_PATTERNS[Networking]="SparkEngine/Source/Engine/Networking/"
SUBSYSTEM_THRESHOLDS[Networking]=10

SUBSYSTEM_PATTERNS[Gameplay]="SparkEngine/Source/Engine/Gameplay/"
SUBSYSTEM_THRESHOLDS[Gameplay]=10

SUBSYSTEM_PATTERNS[SaveSystem]="SparkEngine/Source/Engine/SaveSystem/"
SUBSYSTEM_THRESHOLDS[SaveSystem]=10

SUBSYSTEM_PATTERNS[Events]="SparkEngine/Source/Engine/Events/"
SUBSYSTEM_THRESHOLDS[Events]=15

SUBSYSTEM_PATTERNS[Scripting]="SparkEngine/Source/Engine/Scripting/"
SUBSYSTEM_THRESHOLDS[Scripting]=5

SUBSYSTEM_PATTERNS[UI]="SparkEngine/Source/Engine/UI/"
SUBSYSTEM_THRESHOLDS[UI]=5

SUBSYSTEM_PATTERNS[2D]="SparkEngine/Source/Engine/2D/"
SUBSYSTEM_THRESHOLDS[2D]=10

SUBSYSTEM_PATTERNS[Cinematic]="SparkEngine/Source/Engine/Cinematic/"
SUBSYSTEM_THRESHOLDS[Cinematic]=10

SUBSYSTEM_PATTERNS[Coroutine]="SparkEngine/Source/Engine/Coroutine/"
SUBSYSTEM_THRESHOLDS[Coroutine]=10

SUBSYSTEM_PATTERNS[Dialogue]="SparkEngine/Source/Engine/Dialogue/"
SUBSYSTEM_THRESHOLDS[Dialogue]=10

SUBSYSTEM_PATTERNS[Destruction]="SparkEngine/Source/Engine/Destruction/"
SUBSYSTEM_THRESHOLDS[Destruction]=10

SUBSYSTEM_PATTERNS[Loading]="SparkEngine/Source/Engine/Loading/"
SUBSYSTEM_THRESHOLDS[Loading]=10

SUBSYSTEM_PATTERNS[Localization]="SparkEngine/Source/Engine/Localization/"
SUBSYSTEM_THRESHOLDS[Localization]=10

SUBSYSTEM_PATTERNS[Modding]="SparkEngine/Source/Engine/Modding/"
SUBSYSTEM_THRESHOLDS[Modding]=5

SUBSYSTEM_PATTERNS[Streaming]="SparkEngine/Source/Engine/Streaming/"
SUBSYSTEM_THRESHOLDS[Streaming]=5

SUBSYSTEM_PATTERNS[Replay]="SparkEngine/Source/Engine/Replay/"
SUBSYSTEM_THRESHOLDS[Replay]=5

SUBSYSTEM_PATTERNS[Tween]="SparkEngine/Source/Engine/Tween/"
SUBSYSTEM_THRESHOLDS[Tween]=10

SUBSYSTEM_PATTERNS[VR]="SparkEngine/Source/Engine/VR/"
SUBSYSTEM_THRESHOLDS[VR]=5

SUBSYSTEM_PATTERNS[World]="SparkEngine/Source/Engine/World/"
SUBSYSTEM_THRESHOLDS[World]=10

# Editor
SUBSYSTEM_PATTERNS[Editor]="SparkEditor/Source/"
SUBSYSTEM_THRESHOLDS[Editor]=3

# Game modules
SUBSYSTEM_PATTERNS[GameModules]="GameModules/"
SUBSYSTEM_THRESHOLDS[GameModules]=5

# Console
SUBSYSTEM_PATTERNS[Console]="SparkConsole/src/"
SUBSYSTEM_THRESHOLDS[Console]=5

# SDK
SUBSYSTEM_PATTERNS[SDK]="SparkSDK/"
SUBSYSTEM_THRESHOLDS[SDK]=0

# Global threshold for total project coverage
GLOBAL_THRESHOLD=20

# ============================================================================
# Extract coverage for a path pattern using lcov
# Returns: "lines_hit lines_total percentage"
# ============================================================================
extract_coverage() {
    local pattern="$1"
    local tmpfile
    tmpfile=$(mktemp)

    # Extract only lines matching the pattern
    lcov --extract "$COVERAGE_FILE" "*/${pattern}*" \
        --output-file "$tmpfile" \
        --ignore-errors unused,negative 2>/dev/null || true

    if [[ ! -s "$tmpfile" ]]; then
        rm -f "$tmpfile"
        echo "0 0 0.0"
        return
    fi

    # Parse the summary
    local summary
    summary=$(lcov --summary "$tmpfile" --ignore-errors empty 2>&1 || true)

    local lines_pct lines_hit lines_total
    lines_pct=$(echo "$summary" | grep -oP 'lines\.*:\s*\K[\d.]+' || echo "0.0")
    lines_hit=$(echo "$summary" | grep -oP '\((\d+) of' | grep -oP '\d+' | head -1 || echo "0")
    lines_total=$(echo "$summary" | grep -oP 'of (\d+)' | grep -oP '\d+' | head -1 || echo "0")

    rm -f "$tmpfile"

    # Fallback if parsing failed
    [[ -z "$lines_pct" ]] && lines_pct="0.0"
    [[ -z "$lines_hit" ]] && lines_hit="0"
    [[ -z "$lines_total" ]] && lines_total="0"

    echo "$lines_hit $lines_total $lines_pct"
}

# ============================================================================
# Get total project coverage
# ============================================================================
get_total_coverage() {
    local summary
    summary=$(lcov --summary "$COVERAGE_FILE" --ignore-errors empty 2>&1 || true)

    local lines_pct lines_hit lines_total
    lines_pct=$(echo "$summary" | grep -oP 'lines\.*:\s*\K[\d.]+' || echo "0.0")
    lines_hit=$(echo "$summary" | grep -oP '\((\d+) of' | grep -oP '\d+' | head -1 || echo "0")
    lines_total=$(echo "$summary" | grep -oP 'of (\d+)' | grep -oP '\d+' | head -1 || echo "0")

    [[ -z "$lines_pct" ]] && lines_pct="0.0"
    [[ -z "$lines_hit" ]] && lines_hit="0"
    [[ -z "$lines_total" ]] && lines_total="0"

    echo "$lines_hit $lines_total $lines_pct"
}

# ============================================================================
# Color helpers for terminal output
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

color_pct() {
    local pct="$1"
    local threshold="$2"
    local int_pct=${pct%.*}
    [[ -z "$int_pct" ]] && int_pct=0

    if (( int_pct >= threshold + 20 )); then
        echo -e "${GREEN}${pct}%${RESET}"
    elif (( int_pct >= threshold )); then
        echo -e "${YELLOW}${pct}%${RESET}"
    else
        echo -e "${RED}${pct}%${RESET}"
    fi
}

status_icon() {
    local pct="$1"
    local threshold="$2"
    local int_pct=${pct%.*}
    [[ -z "$int_pct" ]] && int_pct=0

    if (( int_pct >= threshold )); then
        echo "PASS"
    else
        echo "FAIL"
    fi
}

# ============================================================================
# Main report
# ============================================================================

echo ""
echo -e "${BOLD}=================================================================================${RESET}"
echo -e "${BOLD}                    SparkEngine — Code Coverage Report${RESET}"
echo -e "${BOLD}=================================================================================${RESET}"
echo ""

# Total coverage
read -r total_hit total_total total_pct < <(get_total_coverage)
echo -e "${BOLD}Total Project Coverage:${RESET} $(color_pct "$total_pct" "$GLOBAL_THRESHOLD") ($total_hit / $total_total lines)"
echo ""

# Per-subsystem table
printf "${BOLD}%-20s %10s %10s %10s %10s %8s${RESET}\n" \
    "Subsystem" "Lines Hit" "Total" "Coverage" "Threshold" "Status"
printf "%-20s %10s %10s %10s %10s %8s\n" \
    "-------------------" "---------" "---------" "---------" "---------" "------"

FAILURES=0
JSON_ENTRIES=""

# Sort subsystem names alphabetically
SORTED_SUBSYSTEMS=$(echo "${!SUBSYSTEM_PATTERNS[@]}" | tr ' ' '\n' | sort)

for subsystem in $SORTED_SUBSYSTEMS; do
    pattern="${SUBSYSTEM_PATTERNS[$subsystem]}"
    threshold="${SUBSYSTEM_THRESHOLDS[$subsystem]}"

    read -r hit total pct < <(extract_coverage "$pattern")

    status=$(status_icon "$pct" "$threshold")
    if [[ "$status" == "FAIL" ]]; then
        ((FAILURES++)) || true
        status_str="${RED}FAIL${RESET}"
    else
        status_str="${GREEN}PASS${RESET}"
    fi

    printf "%-20s %10s %10s %10s %9s%%   " \
        "$subsystem" "$hit" "$total" "$(color_pct "$pct" "$threshold")" "$threshold"
    echo -e "$status_str"

    # Build JSON
    if [[ -n "$JSON_ENTRIES" ]]; then
        JSON_ENTRIES="${JSON_ENTRIES},"
    fi
    JSON_ENTRIES="${JSON_ENTRIES}
    {\"subsystem\":\"$subsystem\",\"lines_hit\":$hit,\"lines_total\":$total,\"coverage\":$pct,\"threshold\":$threshold,\"pass\":$([ "$status" == "PASS" ] && echo true || echo false)}"
done

printf "%-20s %10s %10s %10s %10s %8s\n" \
    "-------------------" "---------" "---------" "---------" "---------" "------"

# Total row
total_status=$(status_icon "$total_pct" "$GLOBAL_THRESHOLD")
printf "${BOLD}%-20s %10s %10s %10s %9s%%${RESET}\n" \
    "TOTAL" "$total_hit" "$total_total" "$(color_pct "$total_pct" "$GLOBAL_THRESHOLD")" "$GLOBAL_THRESHOLD"

echo ""
echo -e "${BOLD}=================================================================================${RESET}"

# ============================================================================
# JSON output (for CI consumption)
# ============================================================================
if [[ -n "$JSON_FILE" ]]; then
    cat > "$JSON_FILE" <<JSONEOF
{
  "total": {
    "lines_hit": $total_hit,
    "lines_total": $total_total,
    "coverage": $total_pct,
    "threshold": $GLOBAL_THRESHOLD,
    "pass": $([ "$total_status" == "PASS" ] && echo true || echo false)
  },
  "subsystems": [$JSON_ENTRIES
  ]
}
JSONEOF
    echo "JSON report written to: $JSON_FILE"
fi

# ============================================================================
# Threshold enforcement (--check mode)
# ============================================================================
if [[ "$CHECK_MODE" == true ]]; then
    echo ""
    if (( FAILURES > 0 )); then
        echo -e "${RED}${BOLD}COVERAGE CHECK FAILED: $FAILURES subsystem(s) below threshold${RESET}"
        exit 1
    fi

    total_int=${total_pct%.*}
    [[ -z "$total_int" ]] && total_int=0
    if (( total_int < GLOBAL_THRESHOLD )); then
        echo -e "${RED}${BOLD}COVERAGE CHECK FAILED: Total coverage ${total_pct}% below ${GLOBAL_THRESHOLD}% threshold${RESET}"
        exit 1
    fi

    echo -e "${GREEN}${BOLD}COVERAGE CHECK PASSED: All subsystems meet thresholds (total: ${total_pct}%)${RESET}"
fi
