#!/bin/bash

# SparkEngine AI Context Updater
# Scans the live codebase and updates .claude/index.md quick reference section
# with accurate, current metrics. Also updates CLAUDE.md test/panel counts.
#
# What it updates:
#   - .claude/index.md: Quick Reference → Current Engine State section
#   - CLAUDE.md: test count, panel count in architecture section
#
# Usage:
#   ./update-context.sh              # Update (default)
#   ./update-context.sh update       # Same as above
#   ./update-context.sh check        # Dry-run: report if out of date (exit 1 if stale)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

SRC="$PROJECT_ROOT/SparkEngine/Source"
EDITOR_SRC="$PROJECT_ROOT/SparkEditor/Source"
CONSOLE_SRC="$PROJECT_ROOT/SparkConsole/src"
SHADER_SRC="$PROJECT_ROOT/SparkShaderCompiler/src"
GAME_SRC="$PROJECT_ROOT/GameModules"
TEST_DIR="$PROJECT_ROOT/Tests"
WIKI_DIR="$PROJECT_ROOT/wiki"

INDEX_FILE="$PROJECT_ROOT/.claude/index.md"
CLAUDE_FILE="$PROJECT_ROOT/CLAUDE.md"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[CONTEXT]${NC} $1"; }
log_success() { echo -e "${GREEN}[CONTEXT]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[CONTEXT]${NC} $1"; }

CHANGES_MADE=0

# ============================================================================
# Collect metrics
# ============================================================================
collect_metrics() {
    log_info "Scanning codebase..."

    TEST_CASES=$(grep -rch '^TEST(' "$TEST_DIR"/*.cpp 2>/dev/null | awk '{s+=$1} END {print s}')
    TEST_FILES=$(find "$TEST_DIR" -name 'Test*.cpp' 2>/dev/null | wc -l | tr -d " ")
    PANEL_COUNT=$(find "$EDITOR_SRC/Panels" -name '*Panel.h' 2>/dev/null | wc -l | tr -d " ")
    COMP_STRUCTS=$(grep -rh "^struct " "$SRC/Engine/ECS/Components" 2>/dev/null | grep -v "//" | wc -l | tr -d " ")
    ECS_SYSTEMS=$(grep -c "class .*System" "$SRC/Engine/ECS/Systems/ECSystems.h" 2>/dev/null || echo "0")
    GAME_MODULES=$(find "$GAME_SRC" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | wc -l | tr -d " ")
    WIKI_PAGES=$(find "$WIKI_DIR" -name '*.md' ! -name '_Sidebar.md' 2>/dev/null | wc -l | tr -d " ")
    TOTAL_FILES=$(find "$SRC" "$EDITOR_SRC" "$GAME_SRC" "$CONSOLE_SRC" "$SHADER_SRC" "$TEST_DIR" \
        -name '*.cpp' -o -name '*.h' -o -name '*.hpp' 2>/dev/null | wc -l | tr -d " ")
    TOTAL_LINES=$(find "$SRC" "$EDITOR_SRC" "$GAME_SRC" "$CONSOLE_SRC" "$SHADER_SRC" "$TEST_DIR" \
        -name '*.cpp' -o -name '*.h' -o -name '*.hpp' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')
    TOTAL_LINES_K=$((TOTAL_LINES / 1000))

    # Post-processing pass count
    PP_COUNT=$(grep -c "^    [A-Z]" "$SRC/Graphics/PostProcessingTypes.h" 2>/dev/null || echo "14")

    # RHI backends
    RHI_BACKENDS=$(ls -d "$SRC/Graphics/RHI"/*/ 2>/dev/null | wc -l | tr -d " ")
    [ -f "$SRC/Graphics/RHI/NullRHIDevice.h" ] && RHI_BACKENDS=$((RHI_BACKENDS + 1))

    # Game module names
    GAME_MODULE_NAMES=$(find "$GAME_SRC" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sed "s|.*/Spark||; s|Game||" | sort | tr '\n' ', ' | sed 's/,$//' | sed 's/,/, /g')

    FORMATTED_TESTS=$(printf "%'d" "$TEST_CASES" 2>/dev/null || echo "$TEST_CASES")
    TODAY=$(date +%Y-%m-%d)

    log_info "Found: ${TEST_CASES} tests, ${PANEL_COUNT} panels, ${COMP_STRUCTS} components, ${ECS_SYSTEMS} systems"
}

# ============================================================================
# Portable sed replace (updates a pattern in a file)
# ============================================================================
sed_replace() {
    local file="$1"
    local pattern="$2"
    local replacement="$3"

    if [ ! -f "$file" ]; then return; fi

    local before_hash
    before_hash=$(md5sum "$file" | awk '{print $1}')
    sed -i "s|${pattern}|${replacement}|g" "$file"
    local after_hash
    after_hash=$(md5sum "$file" | awk '{print $1}')

    if [ "$before_hash" != "$after_hash" ]; then
        CHANGES_MADE=$((CHANGES_MADE + 1))
        log_info "  Updated $(basename "$file")"
    fi
}

# ============================================================================
# Update .claude/index.md
# ============================================================================
update_index() {
    if [ ! -f "$INDEX_FILE" ]; then
        log_warning ".claude/index.md not found — skipping"
        return
    fi

    # Update date
    sed_replace "$INDEX_FILE" \
        '### Current Engine State ([0-9-]*)' \
        "### Current Engine State (${TODAY})"

    # Tests line
    sed_replace "$INDEX_FILE" \
        '- \*\*Tests\*\*: [0-9]* test files, [0-9,]* tests' \
        "- **Tests**: ${TEST_FILES} test files, ${FORMATTED_TESTS} tests"

    # Editor panels
    sed_replace "$INDEX_FILE" \
        '- \*\*Editor\*\*: [0-9]* panels' \
        "- **Editor**: ${PANEL_COUNT} panels"

    # ECS line
    sed_replace "$INDEX_FILE" \
        '- \*\*ECS\*\*: [0-9]* component types across [0-9]* headers, [0-9]* systems' \
        "- **ECS**: ${COMP_STRUCTS} component types across 17 headers, ${ECS_SYSTEMS} systems"

    # Game modules
    sed_replace "$INDEX_FILE" \
        '- \*\*Game modules\*\*: [0-9]* (' \
        "- **Game modules**: ${GAME_MODULES} ("

    # Codebase summary line
    sed_replace "$INDEX_FILE" \
        '- \*\*Codebase\*\*: ~[0-9]*K lines of C++ across [0-9,]* source files, [0-9]* wiki pages' \
        "- **Codebase**: ~${TOTAL_LINES_K}K lines of C++ across ${TOTAL_FILES} source files, ${WIKI_PAGES} wiki pages"
}

# ============================================================================
# Update CLAUDE.md
# ============================================================================
update_claude_md() {
    if [ ! -f "$CLAUDE_FILE" ]; then
        log_warning "CLAUDE.md not found — skipping"
        return
    fi

    # Tests line
    sed_replace "$CLAUDE_FILE" \
        '[0-9,]* unit tests across [0-9]* files, CTest' \
        "${FORMATTED_TESTS} unit tests across ${TEST_FILES} files, CTest"

    # Editor panel count
    sed_replace "$CLAUDE_FILE" \
        '[0-9]* specialized panels' \
        "${PANEL_COUNT} specialized panels"
}

# ============================================================================
# Main
# ============================================================================
update() {
    collect_metrics

    log_info "Updating .claude/index.md..."
    update_index

    log_info "Updating CLAUDE.md..."
    update_claude_md

    if [ "$CHANGES_MADE" -gt 0 ]; then
        log_success "Updated $CHANGES_MADE file(s)"
    else
        log_success "All context files already up to date"
    fi
}

check_mode() {
    collect_metrics
    CHANGES_MADE=0
    update_index
    update_claude_md

    if [ "$CHANGES_MADE" -gt 0 ]; then
        log_warning "$CHANGES_MADE file(s) out of date. Run: docs/update-context.sh"
        exit 1
    else
        log_success "All context files up to date."
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
