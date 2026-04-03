#!/bin/bash

# SparkEngine Bloat Threshold Checker
# Reports source files exceeding CLAUDE.md thresholds:
#   - .cpp files > 500 lines
#   - .h files > 300 lines
#
# Usage:
#   ./check-bloat.sh              # Report all violations
#   ./check-bloat.sh check        # Same (exit 1 if violations exist)
#   ./check-bloat.sh baseline     # Save current state as baseline
#   ./check-bloat.sh new-only     # Exit 1 only for NEW violations (vs baseline)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BASELINE="$PROJECT_ROOT/.bloat-baseline.txt"

CPP_THRESHOLD=500
H_THRESHOLD=300

SOURCE_DIRS=(
    "$PROJECT_ROOT/SparkEngine/Source"
    "$PROJECT_ROOT/SparkEditor/Source"
    "$PROJECT_ROOT/SparkConsole/src"
    "$PROJECT_ROOT/SparkShaderCompiler/src"
    "$PROJECT_ROOT/GameModules"
)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[BLOAT]${NC} $1"; }
log_success() { echo -e "${GREEN}[BLOAT]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[BLOAT]${NC} $1"; }

CPP_VIOLATIONS=0
H_VIOLATIONS=0

check_files() {
    local ext="$1"
    local threshold="$2"
    local label="$3"
    local count=0

    while read -r lines file; do
        if [ "$lines" -gt "$threshold" ] 2>/dev/null; then
            local rel
            rel=$(echo "$file" | sed "s|$PROJECT_ROOT/||")
            local over=$((lines - threshold))
            printf "  ${YELLOW}%6d${NC} lines (+%d over) — %s\n" "$lines" "$over" "$rel"
            count=$((count + 1))
        fi
    done < <(find "${SOURCE_DIRS[@]}" -name "*.$ext" 2>/dev/null | xargs wc -l 2>/dev/null | grep -v " total$" | sort -rn)

    echo "$count"
}

generate_violation_list() {
    {
        find "${SOURCE_DIRS[@]}" -name "*.cpp" 2>/dev/null | xargs wc -l 2>/dev/null | grep -v " total$" | \
            awk -v t=$CPP_THRESHOLD '{if ($1 > t) print $2}' | sed "s|$PROJECT_ROOT/||" | sort
        find "${SOURCE_DIRS[@]}" -name "*.h" 2>/dev/null | xargs wc -l 2>/dev/null | grep -v " total$" | \
            awk -v t=$H_THRESHOLD '{if ($1 > t) print $2}' | sed "s|$PROJECT_ROOT/||" | sort
    }
}

# ============================================================================
# Main modes
# ============================================================================

run_check() {
    log_info "Checking .cpp files (threshold: $CPP_THRESHOLD lines)..."
    CPP_VIOLATIONS=$(check_files "cpp" "$CPP_THRESHOLD" ".cpp")

    echo ""
    log_info "Checking .h files (threshold: $H_THRESHOLD lines)..."
    H_VIOLATIONS=$(check_files "h" "$H_THRESHOLD" ".h")

    local total=$((CPP_VIOLATIONS + H_VIOLATIONS))
    echo ""

    if [ "$total" -eq 0 ]; then
        log_success "No bloat violations found"
        exit 0
    else
        log_warning "$total file(s) exceed thresholds ($CPP_VIOLATIONS .cpp, $H_VIOLATIONS .h)"
        exit 1
    fi
}

save_baseline() {
    generate_violation_list > "$BASELINE"
    local count
    count=$(wc -l < "$BASELINE")
    log_success "Saved baseline: $count known violations to .bloat-baseline.txt"
}

check_new_only() {
    if [ ! -f "$BASELINE" ]; then
        log_warning "No baseline found. Run: tools/check-bloat.sh baseline"
        run_check
        return
    fi

    local current
    current=$(generate_violation_list)
    local new_violations
    new_violations=$(comm -23 <(echo "$current") <(sort "$BASELINE"))

    if [ -z "$new_violations" ]; then
        local known
        known=$(wc -l < "$BASELINE")
        log_success "No NEW bloat violations ($known known, baselined)"
        exit 0
    else
        local count
        count=$(echo "$new_violations" | wc -l)
        log_warning "$count NEW file(s) exceed thresholds (not in baseline):"
        echo "$new_violations" | while IFS= read -r f; do
            echo "  ${YELLOW}→${NC} $f"
        done
        exit 1
    fi
}

case "${1:-check}" in
    check)     run_check ;;
    baseline)  save_baseline ;;
    new-only)  check_new_only ;;
    *)
        echo "Usage: $0 [check|baseline|new-only]"
        exit 1
        ;;
esac
