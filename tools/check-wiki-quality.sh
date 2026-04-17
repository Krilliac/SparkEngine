#!/bin/bash

# SparkEngine Wiki Quality Checks
# - Detect stale hardcoded metrics in top-level pages
# - Ensure wiki authoring template and contributing guidance exist
#
# Usage:
#   ./check-wiki-quality.sh            # strict (exit 1 on issues)
#   ./check-wiki-quality.sh --warn-only

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WIKI_DIR="$PROJECT_ROOT/wiki"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[WIKI-QUALITY]${NC} $1"; }
log_success() { echo -e "${GREEN}[WIKI-QUALITY]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WIKI-QUALITY]${NC} $1"; }

WARN_ONLY=false
if [ "${1:-check}" = "--warn-only" ]; then
    WARN_ONLY=true
fi

ISSUES=0

check_no_pattern() {
    local file="$1"
    local pattern="$2"
    local description="$3"

    if rg -n "$pattern" "$file" >/dev/null 2>&1; then
        log_warning "$description found in $(basename "$file")"
        rg -n "$pattern" "$file" || true
        ISSUES=$((ISSUES + 1))
    fi
}

log_info "Running wiki quality checks..."

if [ ! -f "$WIKI_DIR/_Template.md" ]; then
    log_warning "Missing wiki/_Template.md"
    ISSUES=$((ISSUES + 1))
fi

CONTRIBUTING_FILE="$WIKI_DIR/advanced/Contributing.md"
if [ ! -f "$CONTRIBUTING_FILE" ]; then
    log_warning "Missing $CONTRIBUTING_FILE"
    ISSUES=$((ISSUES + 1))
elif ! rg -q "Wiki Authoring Standard" "$CONTRIBUTING_FILE"; then
    log_warning "Contributing.md does not include 'Wiki Authoring Standard' section"
    ISSUES=$((ISSUES + 1))
fi

HOME_FILE="$WIKI_DIR/Home.md"
TESTING_FILE="$WIKI_DIR/advanced/Testing.md"

if [ -f "$HOME_FILE" ]; then
    check_no_pattern "$HOME_FILE" "3,119|244 test files" "Stale hardcoded test metrics"
fi

if [ -f "$TESTING_FILE" ]; then
    check_no_pattern "$TESTING_FILE" "3,119|244 test files" "Stale hardcoded test metrics"
fi

if [ "$ISSUES" -eq 0 ]; then
    log_success "Wiki quality checks passed"
    exit 0
fi

if [ "$WARN_ONLY" = true ]; then
    log_warning "$ISSUES issue(s) detected (warn-only mode)"
    exit 0
fi

log_warning "$ISSUES issue(s) detected"
exit 1
