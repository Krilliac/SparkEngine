#!/bin/bash

# SparkEngine Documentation Master Updater
# Runs all documentation generation and sync scripts in the correct order.
# Single command to bring all docs, wikis, badges, and context up to date.
#
# Scripts orchestrated (in order):
#   1. sync-wiki.sh          — Update AUTO: sections in wiki pages
#   2. generate-api-docs.sh  — Regenerate API reference if headers changed
#   3. generate-flowchart.sh — Regenerate architecture flowchart
#   4. update-codebase-stats.sh — Regenerate Codebase-Statistics.md
#   5. update-readme-badges.sh  — Update README.md counts & badge JSON
#   6. update-context.sh        — Update .claude/index.md & CLAUDE.md
#
# Usage:
#   ./update-all-docs.sh              # Run all updates (default)
#   ./update-all-docs.sh update       # Same as above
#   ./update-all-docs.sh check        # Dry-run: report what's out of date (exit 1 if any stale)
#   ./update-all-docs.sh quick        # Skip slow steps (API docs, flowchart)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[ALL-DOCS]${NC} $1"; }
log_success() { echo -e "${GREEN}[ALL-DOCS]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[ALL-DOCS]${NC} $1"; }
log_header()  { echo -e "\n${BOLD}${BLUE}═══ $1 ═══${NC}"; }

FAILURES=0
UPDATES=0

# ============================================================================
# Run a doc script, tracking success/failure
# ============================================================================
run_script() {
    local name="$1"
    local script="$2"
    local mode="$3"

    if [ ! -x "$script" ]; then
        chmod +x "$script" 2>/dev/null || true
    fi

    if [ ! -f "$script" ]; then
        log_warning "Script not found: $script — skipping"
        return
    fi

    log_header "$name"

    if bash "$script" "$mode"; then
        UPDATES=$((UPDATES + 1))
    else
        if [ "$mode" = "check" ]; then
            FAILURES=$((FAILURES + 1))
        else
            log_warning "$name failed"
            FAILURES=$((FAILURES + 1))
        fi
    fi
}

# ============================================================================
# Full update mode
# ============================================================================
update_all() {
    local skip_slow="${1:-false}"

    log_info "Running all documentation updates..."
    echo ""

    # 1. Wiki sync (updates AUTO: sections — must run first so other scripts see fresh data)
    run_script "Wiki Sync" "$SCRIPT_DIR/sync-wiki.sh" "sync"

    # 2. API docs (slow — checksum-based skip if unchanged)
    if [ "$skip_slow" = "false" ]; then
        run_script "API Docs" "$SCRIPT_DIR/generate-api-docs.sh" "check"
        # Only regenerate if check failed (out of date)
        if [ $? -ne 0 ] 2>/dev/null; then
            run_script "API Docs (generate)" "$SCRIPT_DIR/generate-api-docs.sh" "generate"
        fi
    else
        log_info "Skipping API docs (quick mode)"
    fi

    # 3. Architecture flowchart
    if [ "$skip_slow" = "false" ]; then
        run_script "Architecture Flowchart" "$SCRIPT_DIR/generate-flowchart.sh" "generate"
    else
        log_info "Skipping flowchart (quick mode)"
    fi

    # 4. Codebase Statistics page
    run_script "Codebase Statistics" "$SCRIPT_DIR/update-codebase-stats.sh" "generate"

    # 5. README badges & counts
    run_script "README Badges" "$SCRIPT_DIR/update-readme-badges.sh" "update"

    # 6. AI context (.claude/index.md, CLAUDE.md)
    run_script "AI Context" "$SCRIPT_DIR/update-context.sh" "update"

    # Summary
    echo ""
    log_header "Summary"
    if [ "$FAILURES" -eq 0 ]; then
        log_success "All documentation is up to date ($UPDATES scripts ran successfully)"
    else
        log_warning "$FAILURES script(s) had issues, $UPDATES succeeded"
    fi
}

# ============================================================================
# Check mode — report what's stale without modifying anything
# ============================================================================
check_all() {
    log_info "Checking all documentation for staleness..."
    echo ""

    local stale=0

    # Check each script in check/dry-run mode
    for pair in \
        "Wiki Sync:sync-wiki.sh:check" \
        "API Docs:generate-api-docs.sh:check" \
        "Flowchart:generate-flowchart.sh:check" \
        "Codebase Stats:update-codebase-stats.sh:check" \
        "README Badges:update-readme-badges.sh:check" \
        "AI Context:update-context.sh:check"
    do
        local name="${pair%%:*}"
        local rest="${pair#*:}"
        local script="${rest%%:*}"
        local mode="${rest#*:}"
        local full_path="$SCRIPT_DIR/$script"

        if [ ! -f "$full_path" ]; then
            log_warning "$name: script not found ($script)"
            continue
        fi

        echo -ne "${BLUE}[CHECK]${NC} $name... "
        if bash "$full_path" "$mode" > /dev/null 2>&1; then
            echo -e "${GREEN}✓ up to date${NC}"
        else
            echo -e "${YELLOW}✗ out of date${NC}"
            stale=$((stale + 1))
        fi
    done

    echo ""
    if [ "$stale" -eq 0 ]; then
        log_success "All documentation is up to date."
        exit 0
    else
        log_warning "$stale doc source(s) out of date. Run: docs/update-all-docs.sh"
        exit 1
    fi
}

# ============================================================================
# Entry point
# ============================================================================
case "${1:-update}" in
    update|full)
        update_all false
        ;;
    quick)
        update_all true
        ;;
    check)
        check_all
        ;;
    help|--help|-h)
        echo "SparkEngine Documentation Master Updater"
        echo ""
        echo "Usage: $0 [command]"
        echo ""
        echo "Commands:"
        echo "  update    Run all doc updates (default)"
        echo "  quick     Skip slow steps (API docs, flowchart)"
        echo "  check     Dry-run: report what's out of date"
        echo "  help      Show this help"
        echo ""
        echo "Individual scripts:"
        echo "  docs/sync-wiki.sh              Wiki AUTO: sections"
        echo "  docs/generate-api-docs.sh      API reference (~240 pages)"
        echo "  docs/generate-flowchart.sh     Architecture flowchart"
        echo "  docs/update-codebase-stats.sh  Codebase-Statistics.md"
        echo "  docs/update-readme-badges.sh   README badges & counts"
        echo "  docs/update-context.sh         .claude/index.md & CLAUDE.md"
        ;;
    *)
        echo "Usage: $0 [update|quick|check|help]"
        exit 1
        ;;
esac
