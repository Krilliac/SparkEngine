#!/bin/bash

# SparkEngine README Badge & Count Updater
# Scans the live codebase and updates hardcoded counts in README.md,
# .github/copilot-instructions.md, .github/prompts/*, and badge JSON files.
#
# What it updates:
#   - README.md: panel count, game module count, and test count prose
#   - .github/badges/*.json: tests, LOC, and source-file count badges
#   - .github/copilot-instructions.md: test/panel counts
#   - .github/prompts/copilot-instructions.md: test/panel counts
#   - .github/prompts/build-test.prompt.md: test counts
#
# Usage:
#   ./update-readme-badges.sh              # Update (default)
#   ./update-readme-badges.sh update       # Same as above
#   ./update-readme-badges.sh check        # Dry-run: report if out of date (exit 1 if stale)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

SRC="$PROJECT_ROOT/SparkEngine/Source"
EDITOR_SRC="$PROJECT_ROOT/SparkEditor/Source"
CONSOLE_SRC="$PROJECT_ROOT/SparkConsole/src"
SHADER_SRC="$PROJECT_ROOT/SparkShaderCompiler/src"
GAME_SRC="$PROJECT_ROOT/GameModules"
TEST_DIR="$PROJECT_ROOT/Tests"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[BADGES]${NC} $1"; }
log_success() { echo -e "${GREEN}[BADGES]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[BADGES]${NC} $1"; }

CHANGES_MADE=0
DRY_RUN=false

if python3 -c 'import sys' >/dev/null 2>&1; then
    PYTHON=(python3)
elif python -c 'import sys' >/dev/null 2>&1; then
    PYTHON=(python)
elif py -3 -c 'import sys' >/dev/null 2>&1; then
    PYTHON=(py -3)
else
    log_warning "Python 3 is required to collect codebase metrics"
    exit 1
fi

# ============================================================================
# Collect metrics
# ============================================================================
collect_metrics() {
    log_info "Scanning codebase..."

    eval "$("${PYTHON[@]}" "$PROJECT_ROOT/docs/codebase-metrics.py" --shell)"
    TEST_CASES="$TEST_DEFINITIONS"
    format_count() {
        "${PYTHON[@]}" -c 'import sys; print(f"{int(sys.argv[1]):,}")' "$1"
    }
    PANEL_COUNT=$(find "$EDITOR_SRC/Panels" -name '*Panel.h' 2>/dev/null | wc -l | tr -d " ")
    GAME_MODULES=$(find "$GAME_SRC" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | wc -l | tr -d " ")
    WIKI_PAGES=$(find "$PROJECT_ROOT/wiki" -name '*.md' ! -name '_Sidebar.md' 2>/dev/null | wc -l | tr -d " ")

    FORMATTED_TESTS=$(format_count "$TEST_CASES")

    log_info "Found: ${TEST_CASES} tests, ${PANEL_COUNT} panels, ${GAME_MODULES} modules, ${TOTAL_LINES} LOC"
}

# ============================================================================
# Sed-based in-place replacement (portable)
# ============================================================================
sed_replace() {
    local file="$1"
    local pattern="$2"
    local replacement="$3"

    if [ ! -f "$file" ]; then
        return
    fi

    local tmpfile
    tmpfile=$(mktemp)
    sed "s|${pattern}|${replacement}|g" "$file" > "$tmpfile"
    if ! cmp -s "$file" "$tmpfile"; then
        CHANGES_MADE=$((CHANGES_MADE + 1))
        log_info "  Updated $(basename "$file")"
        if [ "$DRY_RUN" = false ]; then
            cp "$tmpfile" "$file"
        fi
    fi
    rm -f "$tmpfile"
}

# ============================================================================
# Update README.md
# ============================================================================
update_readme() {
    local readme="$PROJECT_ROOT/README.md"

    # "N specialized panels:" or "N panels"
    sed_replace "$readme" \
        '[0-9]* specialized panels' \
        "${PANEL_COUNT} specialized panels"

    # "ImGui editor (N panels)"
    sed_replace "$readme" \
        'ImGui editor ([0-9]* panels)' \
        "ImGui editor (${PANEL_COUNT} panels)"

    # Test count in prose: "N unit tests across M files"
    sed_replace "$readme" \
        '[0-9,]* unit tests across [0-9,]* files' \
        "${FORMATTED_TESTS} unit tests across ${TEST_FILES} files"

    # Test count in the repository tree summary
    sed_replace "$readme" \
        'Tests/                 [0-9,]* unit tests, [0-9,]* files' \
        "Tests/                 ${FORMATTED_TESTS} unit tests, ${TEST_FILES} files"

    # Wiki page count in tree
    sed_replace "$readme" \
        '# [0-9]* wiki pages' \
        "# ${WIKI_PAGES} wiki pages"

    # Game module count: "(N modules)"
    sed_replace "$readme" \
        'Game module shared libraries ([0-9]* modules)' \
        "Game module shared libraries (${GAME_MODULES} modules)"
}

# ============================================================================
# Update AI instruction files
# ============================================================================
update_ai_instructions() {
    local files=(
        "$PROJECT_ROOT/.github/copilot-instructions.md"
        "$PROJECT_ROOT/.github/prompts/copilot-instructions.md"
    )

    for f in "${files[@]}"; do
        # "N unit tests across M files"
        sed_replace "$f" \
            '[0-9,]* unit tests across [0-9,]* files' \
            "${FORMATTED_TESTS} unit tests across ${TEST_FILES} files"

        # "ImGui-based editor (N panels)"
        sed_replace "$f" \
            'ImGui-based editor ([0-9]* panels)' \
            "ImGui-based editor (${PANEL_COUNT} panels)"
    done

    # build-test.prompt.md
    local bt="$PROJECT_ROOT/.github/prompts/build-test.prompt.md"
    sed_replace "$bt" \
        '[0-9,]* unit tests across [0-9,]* files' \
        "${FORMATTED_TESTS} unit tests across ${TEST_FILES} files"
}

# ============================================================================
# Update badge JSON files (mirrors loc-counter.yml)
# ============================================================================
update_badges() {
    local badge_dir="$PROJECT_ROOT/.github/badges"
    mkdir -p "$badge_dir"

    local formatted_loc formatted_files
    formatted_loc=$(format_count "$TOTAL_LINES")
    formatted_files=$(format_count "$FILE_COUNT")
    local ts
    if [ "$DRY_RUN" = true ] && [ -f "$badge_dir/loc-breakdown.json" ]; then
        ts=$("${PYTHON[@]}" -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["updated"])' \
            "$badge_dir/loc-breakdown.json")
    else
        ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    fi

    local tests_json='{"schemaVersion":1,"label":"tests","message":"'"$FORMATTED_TESTS"'","color":"brightgreen","cacheSeconds":300}'
    local loc_json='{"schemaVersion":1,"label":"C++ lines of code","message":"'"$formatted_loc"'","color":"blue","logo":"cplusplus","cacheSeconds":300}'
    local files_json='{"schemaVersion":1,"label":"source files","message":"'"$formatted_files"'","color":"green","cacheSeconds":300}'
    local breakdown_json='{"schemaVersion":1,"total":'"$TOTAL_LINES"',"files":'"$FILE_COUNT"',"engine":'"$ENGINE_LINES"',"editor":'"$EDITOR_LINES"',"game":'"$GAME_LINES"',"tests":'"$TEST_LINES"',"tools":'"$TOOL_LINES"',"updated":"'"$ts"'"}'

    for pair in "tests.json:$tests_json" "loc.json:$loc_json" "files.json:$files_json" "loc-breakdown.json:$breakdown_json"; do
        local name="${pair%%:*}"
        local content="${pair#*:}"
        local filepath="$badge_dir/$name"

        local tmpfile
        tmpfile=$(mktemp)
        echo "$content" | "${PYTHON[@]}" -m json.tool --indent 2 > "$tmpfile" 2>/dev/null || echo "$content" > "$tmpfile"
        if [ ! -f "$filepath" ] || ! cmp -s "$filepath" "$tmpfile"; then
            CHANGES_MADE=$((CHANGES_MADE + 1))
            log_info "  Updated $name"
            if [ "$DRY_RUN" = false ]; then
                cp "$tmpfile" "$filepath"
            fi
        fi
        rm -f "$tmpfile"
    done
}

# ============================================================================
# Main
# ============================================================================
update() {
    collect_metrics

    log_info "Updating README.md..."
    update_readme

    log_info "Updating AI instruction files..."
    update_ai_instructions

    log_info "Updating badge JSON files..."
    update_badges

    if [ "$CHANGES_MADE" -gt 0 ]; then
        log_success "Updated $CHANGES_MADE file(s)"
    else
        log_success "All files already up to date"
    fi
}

check_mode() {
    collect_metrics
    DRY_RUN=true
    CHANGES_MADE=0
    update_readme
    update_ai_instructions
    update_badges

    if [ "$CHANGES_MADE" -gt 0 ]; then
        log_warning "$CHANGES_MADE file(s) out of date. Run: docs/update-readme-badges.sh"
        exit 1
    else
        log_success "All README/badge files are up to date."
        exit 0
    fi
}

case "${1:-update}" in
    update|full) update ;;
    check)       check_mode ;;
    *)
        echo "Usage: $0 [update|check]"
        exit 1
        ;;
esac
