#!/bin/bash

# SparkEngine Wiki Navigation Validator
# Verifies all wiki .md files are listed in _Sidebar.md and vice versa.
#
# Usage:
#   ./check-wiki-nav.sh          # Check (default)
#   ./check-wiki-nav.sh check    # Same as above

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WIKI_DIR="$PROJECT_ROOT/wiki"
SIDEBAR="$WIKI_DIR/_Sidebar.md"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[WIKI-NAV]${NC} $1"; }
log_success() { echo -e "${GREEN}[WIKI-NAV]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WIKI-NAV]${NC} $1"; }

ISSUES=0

if [ ! -f "$SIDEBAR" ]; then
    log_warning "_Sidebar.md not found"
    exit 1
fi

log_info "Checking wiki navigation consistency..."

# Extract page names from markdown links in the sidebar. Links now point at
# subfolder paths like `subsystems/Audio.md` — keep only the basename (slug)
# so we can compare against file slugs regardless of which category folder a
# page lives in. External links (http://, ../docs/...) are skipped.
sidebar_pages=$(grep -oP '\]\([^)]+\)' "$SIDEBAR" \
    | sed 's/\](//;s/)$//' \
    | grep -vE '^(https?:|\.\./|#)' \
    | sed -E 's|.*/||; s/\.md$//' \
    | sort -u)

# List actual wiki .md files (recursive into category subfolders), excluding
# internal helper docs prefixed with "_" at the wiki root.
actual_pages=$(find "$WIKI_DIR" -name '*.md' ! -path "$WIKI_DIR/_*.md" -printf '%f\n' \
    | sed 's/\.md$//' | sort -u)

# Pages in wiki/ but not in sidebar
orphaned=$(comm -23 <(echo "$actual_pages") <(echo "$sidebar_pages"))
if [ -n "$orphaned" ]; then
    log_warning "Pages in wiki/ but NOT in _Sidebar.md:"
    while IFS= read -r page; do
        echo -e "  ${YELLOW}→${NC} $page.md"
        ISSUES=$((ISSUES + 1))
    done <<< "$orphaned"
fi

# Pages in sidebar but no matching .md file
missing=$(comm -13 <(echo "$actual_pages") <(echo "$sidebar_pages"))
if [ -n "$missing" ]; then
    log_warning "Pages in _Sidebar.md but NOT in wiki/:"
    while IFS= read -r page; do
        echo -e "  ${RED}✗${NC} $page (linked in sidebar but file missing)"
        ISSUES=$((ISSUES + 1))
    done <<< "$missing"
fi

if [ "$ISSUES" -eq 0 ]; then
    log_success "Wiki navigation is consistent ($(echo "$actual_pages" | wc -l) pages, all in sidebar)"
    exit 0
else
    log_warning "$ISSUES navigation issue(s) found"
    exit 1
fi
