#!/usr/bin/env bash
# scripts/coverage-report.sh — Per-subsystem coverage analysis for SparkEngine
#
# Usage:
#   scripts/coverage-report.sh <coverage.info> [--check] [--json <out.json>]
#
# Reads an lcov coverage.info file and produces a per-subsystem breakdown.
# With --check, enforces minimum thresholds and exits non-zero on failure.
# With --json, writes machine-readable results for CI consumption.
#
# The script parses coverage.info directly (no per-subsystem lcov calls)
# for speed and robustness.

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
# Subsystem definitions — parallel arrays (portable, no associative arrays)
#
# NAMES[i], PATTERNS[i], THRESHOLDS[i] define each subsystem.
# Thresholds are minimum acceptable line-coverage percentages.
# ============================================================================

NAMES=()
PATTERNS=()
THRESHOLDS=()

add() { NAMES+=("$1"); PATTERNS+=("$2"); THRESHOLDS+=("$3"); }

# Engine core
add "Core"         "SparkEngine/Source/Core/"               20
add "Utils"         "SparkEngine/Source/Utils/"              15
add "Camera"        "SparkEngine/Source/Camera/"             5
add "Audio"         "SparkEngine/Source/Audio/"              3
add "Graphics"      "SparkEngine/Source/Graphics/"           3
add "Input"         "SparkEngine/Source/Input/"              5
add "Physics"       "SparkEngine/Source/Physics/"            5
add "SceneManager"  "SparkEngine/Source/SceneManager/"       3

# Engine subsystems
add "AI"            "SparkEngine/Source/Engine/AI/"          5
add "Animation"     "SparkEngine/Source/Engine/Animation/"   5
add "ECS"           "SparkEngine/Source/Engine/ECS/"         10
add "Networking"    "SparkEngine/Source/Engine/Networking/"   5
add "Gameplay"      "SparkEngine/Source/Engine/Gameplay/"     5
add "SaveSystem"    "SparkEngine/Source/Engine/SaveSystem/"   5
add "Events"        "SparkEngine/Source/Engine/Events/"       10
add "Scripting"     "SparkEngine/Source/Engine/Scripting/"    3
add "UI"            "SparkEngine/Source/Engine/UI/"           3
add "2D"            "SparkEngine/Source/Engine/2D/"           5
add "Cinematic"     "SparkEngine/Source/Engine/Cinematic/"    5
add "Coroutine"     "SparkEngine/Source/Engine/Coroutine/"    5
add "Dialogue"      "SparkEngine/Source/Engine/Dialogue/"     5
add "Destruction"   "SparkEngine/Source/Engine/Destruction/"  5
add "Loading"       "SparkEngine/Source/Engine/Loading/"      5
add "Localization"  "SparkEngine/Source/Engine/Localization/" 5
add "Modding"       "SparkEngine/Source/Engine/Modding/"      3
add "Streaming"     "SparkEngine/Source/Engine/Streaming/"    3
add "Replay"        "SparkEngine/Source/Engine/Replay/"       3
add "Tween"         "SparkEngine/Source/Engine/Tween/"        5
add "VR"            "SparkEngine/Source/Engine/VR/"           3
add "World"         "SparkEngine/Source/Engine/World/"        5

# Editor
add "Editor"        "SparkEditor/Source/"                     2
# Game modules
add "GameModules"   "GameModules/"                            3
# Console
add "Console"       "SparkConsole/src/"                       3
# SDK
add "SDK"           "SparkSDK/"                               0

# Global threshold for total project coverage
GLOBAL_THRESHOLD=15

SUBSYSTEM_COUNT=${#NAMES[@]}

# ============================================================================
# Parse coverage.info into per-file hit/total counts
#
# LCOV info format:
#   SF:<source-file>        — start of a file record
#   DA:<line>,<hit-count>   — line data (hit-count 0 = not covered)
#   end_of_record           — end of file record
#
# We accumulate hits/total per file, then group by subsystem pattern.
# ============================================================================

declare -a SUB_HIT SUB_TOTAL
for (( i=0; i<SUBSYSTEM_COUNT; i++ )); do
    SUB_HIT[i]=0
    SUB_TOTAL[i]=0
done

TOTAL_HIT=0
TOTAL_TOTAL=0

# Read coverage.info once, accumulate per-subsystem
current_file=""
file_hit=0
file_total=0

flush_file() {
    if [[ -z "$current_file" ]]; then
        return
    fi
    TOTAL_HIT=$(( TOTAL_HIT + file_hit ))
    TOTAL_TOTAL=$(( TOTAL_TOTAL + file_total ))

    # Match against each subsystem pattern
    for (( i=0; i<SUBSYSTEM_COUNT; i++ )); do
        if [[ "$current_file" == *"${PATTERNS[i]}"* ]]; then
            SUB_HIT[i]=$(( SUB_HIT[i] + file_hit ))
            SUB_TOTAL[i]=$(( SUB_TOTAL[i] + file_total ))
            break  # Each file belongs to at most one subsystem
        fi
    done

    file_hit=0
    file_total=0
}

while IFS= read -r line; do
    case "$line" in
        SF:*)
            flush_file
            current_file="${line#SF:}"
            ;;
        DA:*)
            # DA:<line-number>,<execution-count>
            count="${line#DA:*,}"
            file_total=$(( file_total + 1 ))
            if [[ "$count" != "0" ]]; then
                file_hit=$(( file_hit + 1 ))
            fi
            ;;
        end_of_record)
            flush_file
            current_file=""
            ;;
    esac
done < "$COVERAGE_FILE"
flush_file  # Handle last file if no trailing end_of_record

# ============================================================================
# Compute percentages
# ============================================================================

calc_pct() {
    local hit="$1" total="$2"
    if (( total == 0 )); then
        echo "0.0"
    else
        # Use awk for floating-point division
        awk "BEGIN { printf \"%.1f\", ($hit / $total) * 100 }"
    fi
}

TOTAL_PCT=$(calc_pct "$TOTAL_HIT" "$TOTAL_TOTAL")

# ============================================================================
# Color helpers for terminal output
# ============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
RESET='\033[0m'

# Disable colors if not a terminal (CI logs)
if [[ ! -t 1 ]]; then
    RED="" GREEN="" YELLOW="" BOLD="" RESET=""
fi

color_pct() {
    local pct="$1" threshold="$2"
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

# ============================================================================
# Main report
# ============================================================================

echo ""
echo -e "${BOLD}=================================================================================${RESET}"
echo -e "${BOLD}                    SparkEngine — Code Coverage Report${RESET}"
echo -e "${BOLD}=================================================================================${RESET}"
echo ""
echo -e "${BOLD}Total Project Coverage:${RESET} $(color_pct "$TOTAL_PCT" "$GLOBAL_THRESHOLD") ($TOTAL_HIT / $TOTAL_TOTAL lines)"
echo ""

printf "${BOLD}%-20s %10s %10s %10s %10s %8s${RESET}\n" \
    "Subsystem" "Lines Hit" "Total" "Coverage" "Threshold" "Status"
printf "%-20s %10s %10s %10s %10s %8s\n" \
    "-------------------" "---------" "---------" "---------" "---------" "------"

FAILURES=0
JSON_ENTRIES=""

# Build sorted index array
SORTED_INDICES=()
for (( i=0; i<SUBSYSTEM_COUNT; i++ )); do
    SORTED_INDICES+=("$i")
done
# Sort by name using a simple selection sort (small array, no need for fancy sort)
for (( i=0; i<SUBSYSTEM_COUNT-1; i++ )); do
    for (( j=i+1; j<SUBSYSTEM_COUNT; j++ )); do
        if [[ "${NAMES[${SORTED_INDICES[i]}]}" > "${NAMES[${SORTED_INDICES[j]}]}" ]]; then
            tmp="${SORTED_INDICES[i]}"
            SORTED_INDICES[i]="${SORTED_INDICES[j]}"
            SORTED_INDICES[j]="$tmp"
        fi
    done
done

for idx in "${SORTED_INDICES[@]}"; do
    name="${NAMES[idx]}"
    hit="${SUB_HIT[idx]}"
    total="${SUB_TOTAL[idx]}"
    threshold="${THRESHOLDS[idx]}"
    pct=$(calc_pct "$hit" "$total")

    int_pct=${pct%.*}
    [[ -z "$int_pct" ]] && int_pct=0

    if (( total > 0 && int_pct < threshold )); then
        status="FAIL"
        ((FAILURES++)) || true
        status_str="${RED}FAIL${RESET}"
    else
        status="PASS"
        status_str="${GREEN}PASS${RESET}"
    fi

    printf "%-20s %10s %10s %10s %9s%%   " \
        "$name" "$hit" "$total" "$(color_pct "$pct" "$threshold")" "$threshold"
    echo -e "$status_str"

    # Build JSON
    if [[ -n "$JSON_ENTRIES" ]]; then
        JSON_ENTRIES="${JSON_ENTRIES},"
    fi
    pass_val="true"
    [[ "$status" == "FAIL" ]] && pass_val="false"
    JSON_ENTRIES="${JSON_ENTRIES}
    {\"subsystem\":\"$name\",\"lines_hit\":$hit,\"lines_total\":$total,\"coverage\":$pct,\"threshold\":$threshold,\"pass\":$pass_val}"
done

printf "%-20s %10s %10s %10s %10s %8s\n" \
    "-------------------" "---------" "---------" "---------" "---------" "------"

printf "${BOLD}%-20s %10s %10s %10s %9s%%${RESET}\n" \
    "TOTAL" "$TOTAL_HIT" "$TOTAL_TOTAL" "$(color_pct "$TOTAL_PCT" "$GLOBAL_THRESHOLD")" "$GLOBAL_THRESHOLD"

echo ""
echo -e "${BOLD}=================================================================================${RESET}"

# ============================================================================
# JSON output (for CI consumption)
# ============================================================================
if [[ -n "$JSON_FILE" ]]; then
    total_pass="true"
    total_int=${TOTAL_PCT%.*}
    [[ -z "$total_int" ]] && total_int=0
    (( total_int < GLOBAL_THRESHOLD )) && total_pass="false"

    cat > "$JSON_FILE" <<JSONEOF
{
  "total": {
    "lines_hit": $TOTAL_HIT,
    "lines_total": $TOTAL_TOTAL,
    "coverage": $TOTAL_PCT,
    "threshold": $GLOBAL_THRESHOLD,
    "pass": $total_pass
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

    total_int=${TOTAL_PCT%.*}
    [[ -z "$total_int" ]] && total_int=0
    if (( total_int < GLOBAL_THRESHOLD )); then
        echo -e "${RED}${BOLD}COVERAGE CHECK FAILED: Total coverage ${TOTAL_PCT}% below ${GLOBAL_THRESHOLD}% threshold${RESET}"
        exit 1
    fi

    echo -e "${GREEN}${BOLD}COVERAGE CHECK PASSED: All subsystems meet thresholds (total: ${TOTAL_PCT}%)${RESET}"
fi
