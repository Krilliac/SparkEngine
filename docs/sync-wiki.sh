#!/bin/bash

# SparkEngine Wiki Synchronization Script
# Scans the codebase and updates wiki pages with accurate, current information.
# Designed to run in any environment (no doxygen/graphviz needed).
#
# What it does:
#   1. Scans all source directories for headers, components, systems, panels, tests
#   2. Updates auto-generated sections in existing wiki pages (between markers)
#   3. Updates Home.md feature list and statistics
#   4. Updates _Sidebar.md if new wiki pages are added
#   5. Flags wiki pages that reference files/classes that no longer exist
#
# Usage:
#   ./sync-wiki.sh              # Full sync (default)
#   ./sync-wiki.sh check        # Dry-run: report what's out of date
#   ./sync-wiki.sh status       # Show sync status

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WIKI_DIR="$PROJECT_ROOT/wiki"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[WIKI-SYNC]${NC} $1"; }
log_success() { echo -e "${GREEN}[WIKI-SYNC]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WIKI-SYNC]${NC} $1"; }
log_error()   { echo -e "${RED}[WIKI-SYNC]${NC} $1"; }

CHANGES_MADE=0
WARNINGS=0
CHECK_MODE=false

# ============================================================================
# Helper: Update a block between markers in a file
# Markers: <!-- AUTO:section_name --> ... <!-- /AUTO:section_name -->
# ============================================================================
update_auto_section() {
    local file="$1"
    local section="$2"
    local content="$3"

    local start_marker="<!-- AUTO:${section} -->"
    local end_marker="<!-- /AUTO:${section} -->"

    if ! grep -qF "$start_marker" "$file" 2>/dev/null; then
        return 1  # Section not found
    fi

    # Build the replacement
    local tmpfile
    tmpfile=$(mktemp)

    awk -v start="$start_marker" -v end="$end_marker" -v body="$content" '
        $0 ~ start { print; print body; skip=1; next }
        $0 ~ end   { skip=0 }
        !skip       { print }
    ' "$file" > "$tmpfile"

    if ! diff -q "$file" "$tmpfile" > /dev/null 2>&1; then
        cp "$tmpfile" "$file"
        CHANGES_MADE=$((CHANGES_MADE + 1))
        log_info "  Updated [$section] in $(basename "$file")"
    fi

    rm -f "$tmpfile"
    return 0
}

# ============================================================================
# Collect codebase inventory
# ============================================================================
collect_inventory() {
    # --- Components ---
    COMPONENT_LIST=""
    local tmpfile
    tmpfile=$(mktemp)

    find "$PROJECT_ROOT/SparkEngine/Source" -name '*Components.h' 2>/dev/null | sort > "$tmpfile"
    while IFS= read -r cfile; do
        [ -z "$cfile" ] && continue
        local rel="${cfile#$PROJECT_ROOT/}"
        local structs
        structs=$(grep -E '^\s*struct\s+[A-Z][A-Za-z0-9]+' "$cfile" 2>/dev/null | \
            sed 's/.*struct\s\+//' | sed 's/[:{; ].*//' | tr -d ' ' | sort)
        [ -z "$structs" ] && continue
        while IFS= read -r s; do
            [ -z "$s" ] && continue
            COMPONENT_LIST="${COMPONENT_LIST}${s}|${rel}\n"
        done <<< "$structs"
    done < "$tmpfile"

    # --- Systems ---
    SYSTEM_LIST=""
    find "$PROJECT_ROOT/SparkEngine/Source" \( -name '*Systems*.h' -o -name '*System.h' \) 2>/dev/null | sort > "$tmpfile"
    while IFS= read -r sfile; do
        [ -z "$sfile" ] && continue
        local rel="${sfile#$PROJECT_ROOT/}"
        local classes
        classes=$(grep -E '^\s*class\s+[A-Z][A-Za-z0-9]*System' "$sfile" 2>/dev/null | \
            sed 's/.*class\s\+//' | sed 's/[:{; ].*//' | tr -d ' ' | sort)
        [ -z "$classes" ] && continue
        while IFS= read -r c; do
            [ -z "$c" ] && continue
            SYSTEM_LIST="${SYSTEM_LIST}${c}|${rel}\n"
        done <<< "$classes"
    done < "$tmpfile"

    # --- Editor Panels ---
    PANEL_LIST=""
    find "$PROJECT_ROOT/SparkEditor/Source/Panels" -name '*.h' 2>/dev/null | sort > "$tmpfile"
    while IFS= read -r pfile; do
        [ -z "$pfile" ] && continue
        local rel="${pfile#$PROJECT_ROOT/}"
        local pname
        pname=$(grep -E '^\s*class\s+[A-Z][A-Za-z0-9]*Panel' "$pfile" 2>/dev/null | \
            sed 's/.*class\s\+//' | sed 's/[:{; ].*//' | tr -d ' ' | head -1)
        [ -z "$pname" ] && continue
        PANEL_LIST="${PANEL_LIST}${pname}|${rel}\n"
    done < "$tmpfile"

    # --- Tests ---
    TEST_COUNT=0
    TEST_FILES=""
    find "$PROJECT_ROOT/Tests" -name 'Test*.cpp' ! -name 'TestMain.cpp' ! -name 'TestFramework*' 2>/dev/null | sort > "$tmpfile"
    while IFS= read -r tfile; do
        [ -z "$tfile" ] && continue
        local tname
        tname=$(basename "$tfile" .cpp)
        TEST_FILES="${TEST_FILES}${tname}\n"
        local tc
        tc=$(grep -c '^TEST(' "$tfile" 2>/dev/null) || tc=0
        TEST_COUNT=$((TEST_COUNT + tc))
    done < "$tmpfile"
    TEST_FILE_COUNT=$(echo -e "$TEST_FILES" | grep -c '[A-Z]' || true)

    rm -f "$tmpfile"

    # --- Header count ---
    HEADER_COUNT=0
    while IFS= read -r header_dir; do
        [ -d "$header_dir" ] || continue
        HEADER_COUNT=$((HEADER_COUNT + $(find "$header_dir" -name '*.h' | wc -l)))
    done < <(
        printf '%s\n' \
            "$PROJECT_ROOT/SparkEngine/Source" \
            "$PROJECT_ROOT/SparkEditor/Source" \
            "$PROJECT_ROOT/SparkConsole/src" \
            "$PROJECT_ROOT/SparkShaderCompiler/src" \
            "$PROJECT_ROOT/SparkSDK"
        find "$PROJECT_ROOT/GameModules" -mindepth 2 -maxdepth 2 -type d -name Source 2>/dev/null | sort
    )

    # --- Wiki pages ---
    WIKI_PAGE_COUNT=$(find "$WIKI_DIR" -name '*.md' ! -name '_Sidebar.md' | wc -l)
}

# ============================================================================
# Sync: Entity-Component-System.md
# ============================================================================
sync_ecs_page() {
    local page="$WIKI_DIR/Entity-Component-System.md"
    [ -f "$page" ] || return 0

    # Check if auto-markers exist; if not, append them
    if ! grep -qF "<!-- AUTO:component_list -->" "$page"; then
        {
            echo ""
            echo "## Component Inventory"
            echo ""
            echo "<!-- AUTO:component_list -->"
            echo "<!-- /AUTO:component_list -->"
            echo ""
            echo "## System Inventory"
            echo ""
            echo "<!-- AUTO:system_list -->"
            echo "<!-- /AUTO:system_list -->"
        } >> "$page"
        CHANGES_MADE=$((CHANGES_MADE + 1))
        log_info "  Added auto-sync markers to Entity-Component-System.md"
    fi

    # Build component table
    local comp_content=""
    comp_content="| Component | Header |\n|-----------|--------|\n"
    comp_content+=$(echo -e "$COMPONENT_LIST" | grep -v '^$' | sort | while IFS='|' read -r name header; do
        echo "| \`$name\` | \`$header\` |"
    done)

    update_auto_section "$page" "component_list" "$(echo -e "$comp_content")"

    # Build system table
    local sys_content=""
    sys_content="| System | Header |\n|--------|--------|\n"
    sys_content+=$(echo -e "$SYSTEM_LIST" | grep -v '^$' | sort | while IFS='|' read -r name header; do
        echo "| \`$name\` | \`$header\` |"
    done)

    update_auto_section "$page" "system_list" "$(echo -e "$sys_content")"
}

# ============================================================================
# Sync: Testing.md
# ============================================================================
sync_testing_page() {
    local page="$WIKI_DIR/Testing.md"
    [ -f "$page" ] || return 0

    if ! grep -qF "<!-- AUTO:test_inventory -->" "$page"; then
        {
            echo ""
            echo "## Test File Inventory"
            echo ""
            echo "<!-- AUTO:test_inventory -->"
            echo "<!-- /AUTO:test_inventory -->"
        } >> "$page"
        CHANGES_MADE=$((CHANGES_MADE + 1))
        log_info "  Added auto-sync markers to Testing.md"
    fi

    local test_content=""
    test_content="*${TEST_FILE_COUNT} test files, ${TEST_COUNT}+ test cases*\n\n"
    test_content+="| Test File | Test Cases |\n|-----------|------------|\n"
    test_content+=$(find "$PROJECT_ROOT/Tests" -name 'Test*.cpp' ! -name 'TestMain.cpp' ! -name 'TestFramework*' 2>/dev/null | \
        sort | while IFS= read -r tfile; do
            local tname
            tname=$(basename "$tfile" .cpp)
            local tc
            tc=$(grep -c '^TEST(' "$tfile" 2>/dev/null) || tc=0
            echo "| \`$tname\` | $tc |"
        done)

    update_auto_section "$page" "test_inventory" "$(echo -e "$test_content")"
}

# ============================================================================
# Sync: SparkEditor.md
# ============================================================================
sync_editor_page() {
    local page="$WIKI_DIR/SparkEditor.md"
    [ -f "$page" ] || return 0

    if ! grep -qF "<!-- AUTO:panel_list -->" "$page"; then
        {
            echo ""
            echo "## Editor Panels"
            echo ""
            echo "<!-- AUTO:panel_list -->"
            echo "<!-- /AUTO:panel_list -->"
        } >> "$page"
        CHANGES_MADE=$((CHANGES_MADE + 1))
        log_info "  Added auto-sync markers to SparkEditor.md"
    fi

    local panel_content=""
    panel_content="| Panel | Header |\n|-------|--------|\n"
    panel_content+=$(echo -e "$PANEL_LIST" | grep -v '^$' | sort | while IFS='|' read -r name header; do
        echo "| \`$name\` | \`$header\` |"
    done)

    update_auto_section "$page" "panel_list" "$(echo -e "$panel_content")"
}

# ============================================================================
# Sync: Home.md stats block
# ============================================================================
sync_home_page() {
    local page="$WIKI_DIR/Home.md"
    [ -f "$page" ] || return 0

    if ! grep -qF "<!-- AUTO:stats -->" "$page"; then
        {
            echo ""
            echo "## Project Statistics"
            echo ""
            echo "<!-- AUTO:stats -->"
            echo "<!-- /AUTO:stats -->"
        } >> "$page"
        CHANGES_MADE=$((CHANGES_MADE + 1))
        log_info "  Added auto-sync markers to Home.md"
    fi

    local comp_count
    comp_count=$(echo -e "$COMPONENT_LIST" | grep -c '[A-Z]' || true)
    local sys_count
    sys_count=$(echo -e "$SYSTEM_LIST" | grep -c '[A-Z]' || true)
    local panel_count
    panel_count=$(echo -e "$PANEL_LIST" | grep -c '[A-Z]' || true)

    local last_synced
    if [ "$CHECK_MODE" = true ]; then
        last_synced=$(grep -E '^\| \*Last synced\* \| \*.*\* \|$' "$page" | sed -E 's/^\| \*Last synced\* \| \*(.*)\* \|$/\1/' | head -1)
        [ -n "$last_synced" ] || last_synced="N/A"
    else
        last_synced="$(date '+%Y-%m-%d %H:%M')"
    fi

    local stats_content
    stats_content="| Metric | Count |
|--------|-------|
| Header files | ${HEADER_COUNT} |
| ECS Components | ${comp_count} |
| Engine System Classes | ${sys_count} |
| Editor Panels | ${panel_count} |
| Test files | ${TEST_FILE_COUNT} |
| Test cases | ${TEST_COUNT}+ |
| Wiki pages | ${WIKI_PAGE_COUNT} |
| *Last synced* | *${last_synced}* |"

    update_auto_section "$page" "stats" "$stats_content"
}

# ============================================================================
# Check: find wiki references to files/classes that no longer exist
# ============================================================================
check_stale_references() {
    log_info "Checking for stale references in wiki..."

    find "$WIKI_DIR" -name '*.md' | while IFS= read -r wfile; do
        # Look for backtick-quoted paths like `SparkEngine/Source/Foo/Bar.h`
        grep -oE '`(SparkEngine|SparkEditor|SparkGame|Tests)/[^`]+\.(h|cpp)`' "$wfile" 2>/dev/null | \
            tr -d '`' | while read -r ref_path; do
                if [ ! -f "$PROJECT_ROOT/$ref_path" ]; then
                    log_warning "  $(basename "$wfile"): references missing file \`$ref_path\`"
                    WARNINGS=$((WARNINGS + 1))
                fi
            done
    done
}

# ============================================================================
# Ensure _Sidebar.md is current
# ============================================================================
sync_sidebar() {
    local sidebar="$WIKI_DIR/_Sidebar.md"
    [ -f "$sidebar" ] || return 0

    # Check if any wiki .md files exist that aren't in the sidebar
    local missing=0
    find "$WIKI_DIR" -name '*.md' ! -name '_*.md' | while IFS= read -r wfile; do
        local page_name
        page_name=$(basename "$wfile" .md)
        if ! grep -qF "$page_name" "$sidebar" 2>/dev/null; then
            log_warning "  Wiki page '$page_name' is not listed in _Sidebar.md"
            WARNINGS=$((WARNINGS + 1))
        fi
    done
}

# ============================================================================
# Main
# ============================================================================
main() {
    local command="${1:-sync}"

    case "$command" in
        sync|full)
            log_info "Synchronizing wiki with codebase..."
            echo ""

            collect_inventory

            sync_ecs_page
            sync_testing_page
            sync_editor_page
            sync_home_page
            sync_sidebar
            check_stale_references

            echo ""
            if [ "$CHANGES_MADE" -gt 0 ]; then
                log_success "Wiki sync complete: $CHANGES_MADE page(s) updated"
            else
                log_success "Wiki is already up to date"
            fi
            if [ "$WARNINGS" -gt 0 ]; then log_warning "$WARNINGS warning(s) — review above"; fi
            ;;

        check)
            log_info "Dry-run: checking wiki freshness..."
            echo ""
            CHECK_MODE=true

            collect_inventory
            check_stale_references
            sync_sidebar

            # Check if any auto-sections would change
            local tmpdir
            tmpdir=$(mktemp -d)
            cp -r "$WIKI_DIR"/*.md "$tmpdir/" 2>/dev/null || true

            sync_ecs_page
            sync_testing_page
            sync_editor_page
            sync_home_page

            local stale=0
            for mdfile in "$WIKI_DIR"/*.md; do
                local bname
                bname=$(basename "$mdfile")
                if [ -f "$tmpdir/$bname" ] && ! diff -q "$mdfile" "$tmpdir/$bname" > /dev/null 2>&1; then
                    log_warning "  $bname is out of date"
                    stale=$((stale + 1))
                fi
            done

            # Restore originals
            cp "$tmpdir"/*.md "$WIKI_DIR/" 2>/dev/null || true
            rm -rf "$tmpdir"

            echo ""
            if [ "$stale" -gt 0 ]; then
                log_warning "$stale page(s) need updating. Run: docs/sync-wiki.sh sync"
                exit 1
            else
                log_success "Wiki is up to date"
            fi
            if [ "$WARNINGS" -gt 0 ]; then log_warning "$WARNINGS warning(s) — review above"; fi
            ;;

        status)
            collect_inventory

            local comp_count
            comp_count=$(echo -e "$COMPONENT_LIST" | grep -c '[A-Z]' || true)
            local sys_count
            sys_count=$(echo -e "$SYSTEM_LIST" | grep -c '[A-Z]' || true)
            local panel_count
            panel_count=$(echo -e "$PANEL_LIST" | grep -c '[A-Z]' || true)

            echo "======================================================"
            log_info "SparkEngine Wiki Sync Status"
            echo "======================================================"
            echo ""
            log_info "Codebase inventory:"
            echo "  Headers:      $HEADER_COUNT"
            echo "  Components:   $comp_count"
            echo "  System classes: $sys_count"
            echo "  Panels:       $panel_count"
            echo "  Test files:   $TEST_FILE_COUNT"
            echo "  Test cases:   $TEST_COUNT+"
            echo ""
            log_info "Wiki:"
            echo "  Pages:        $WIKI_PAGE_COUNT"
            echo "  Directory:    $WIKI_DIR"
            echo "======================================================"
            ;;

        help|-h|--help)
            echo "SparkEngine Wiki Synchronization Script"
            echo ""
            echo "Usage: $0 [sync|check|status|help]"
            echo ""
            echo "  sync    Update wiki pages with current codebase data (default)"
            echo "  check   Dry-run: report what's out of date (exits 1 if stale)"
            echo "  status  Show codebase and wiki statistics"
            echo "  help    Show this message"
            echo ""
            echo "Auto-synced wiki pages:"
            echo "  Entity-Component-System.md  — component & system tables"
            echo "  Testing.md                  — test file inventory"
            echo "  SparkEditor.md              — editor panel list"
            echo "  Home.md                     — project statistics"
            echo "  _Sidebar.md                 — checks for unlisted pages"
            ;;

        *)
            log_error "Unknown command: $command"
            main help
            exit 1
            ;;
    esac
}

main "$@"
