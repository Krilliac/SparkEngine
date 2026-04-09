#!/bin/bash

# SparkEngine Validation Master Script
# Runs all code quality and integrity validation scripts.
#
# Usage:
#   ./validate-all.sh              # Run all checks (exit 1 on failure)
#   ./validate-all.sh check        # Same as above
#   ./validate-all.sh --warn-only  # Report issues but always exit 0
#   ./validate-all.sh help         # Show help

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[VALIDATE]${NC} $1"; }
log_success() { echo -e "${GREEN}[VALIDATE]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[VALIDATE]${NC} $1"; }
log_header()  { echo -e "\n${BOLD}${BLUE}─── $1 ───${NC}"; }

WARN_ONLY=false
case "${1:-check}" in
    --warn-only) WARN_ONLY=true ;;
    check)       WARN_ONLY=false ;;
    help|--help|-h)
        echo "SparkEngine Validation Master Script"
        echo ""
        echo "Usage: $0 [command]"
        echo ""
        echo "Commands:"
        echo "  check        Run all checks, exit 1 on failure (default)"
        echo "  --warn-only  Report issues but always exit 0"
        echo "  help         Show this help"
        echo ""
        echo "Individual scripts:"
        echo "  tools/check-pragma-once.sh       Header #pragma once enforcement"
        echo "  tools/check-editor-panels.sh     Editor panel registration"
        echo "  tools/check-deprecated-submodules.sh Deprecated submodule guardrail"
        echo "  tools/check-thirdparty-manifest-sync.sh Third-party manifest sync"
        echo "  tools/check-wiki-nav.sh          Wiki sidebar consistency"
        echo "  tools/check-wiring.sh            System wiring validation"
        echo "  tools/check-bloat.sh             File size threshold checker (baseline-aware)"
        echo "  tools/check-doxygen-coverage.sh  Doxygen comment coverage"
        echo "  tools/check-cross-utilization.sh Architectural dependency boundaries"
        echo "  tools/check-di-singletons.sh      DI singleton guardrails"
        exit 0
        ;;
esac

PASSED=0
FAILED=0
SKIPPED=0

run_check() {
    local name="$1"
    local script="$2"
    shift 2
    local args=("$@")

    local full_path="$SCRIPT_DIR/$script"

    if [ ! -f "$full_path" ]; then
        log_warning "$name: script not found ($script)"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    chmod +x "$full_path" 2>/dev/null || true

    log_header "$name"

    if bash "$full_path" "${args[@]}" 2>&1; then
        PASSED=$((PASSED + 1))
    else
        FAILED=$((FAILED + 1))
    fi
}

log_info "Running all validation checks..."

# Fast checks first, slower checks later
run_check "Header Guards (#pragma once)" "check-pragma-once.sh"
run_check "Editor Panel Registration"    "check-editor-panels.sh"
run_check "Deprecated Submodules"       "check-deprecated-submodules.sh"
run_check "ThirdParty Manifest Sync"    "check-thirdparty-manifest-sync.sh"
run_check "Wiki Navigation"              "check-wiki-nav.sh"
run_check "System Wiring"                "check-wiring.sh"
run_check "Bloat Thresholds (New Only)"  "check-bloat.sh" "new-only"
run_check "Doxygen Coverage"             "check-doxygen-coverage.sh" "check"
run_check "Cross-Utilization Boundaries" "check-cross-utilization.sh"
run_check "DI Singleton Guardrails"     "check-di-singletons.sh"

# Summary
echo ""
log_header "Summary"

echo -e "  ${GREEN}✓ Passed:${NC}  $PASSED"
echo -e "  ${RED}✗ Failed:${NC}  $FAILED"
if [ "$SKIPPED" -gt 0 ]; then
    echo -e "  ${YELLOW}○ Skipped:${NC} $SKIPPED"
fi
echo ""

if [ "$FAILED" -eq 0 ]; then
    log_success "All validation checks passed"
    exit 0
elif [ "$WARN_ONLY" = true ]; then
    log_warning "$FAILED check(s) failed (warn-only mode — not blocking)"
    exit 0
else
    log_warning "$FAILED check(s) failed"
    exit 1
fi
