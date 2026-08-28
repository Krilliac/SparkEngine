#!/bin/bash
# SparkEngine documentation master updater and exact-currentness gate.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

find_python() {
    local candidate
    for candidate in "${PYTHON:-}" python3 python py; do
        [ -n "$candidate" ] || continue
        if command -v "$candidate" >/dev/null 2>&1 && "$candidate" -c "import sys" >/dev/null 2>&1; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

PYTHON_BIN="$(find_python)" || { echo "Python 3 is required" >&2; exit 1; }
export PYTHON="$PYTHON_BIN"
MODE="${1:-update}"

resolve_identity() {
    SOURCE_SHA="${SPARKENGINE_DOC_SOURCE_SHA:-}"
    SOURCE_COMMITTED_AT="${SPARKENGINE_DOC_SOURCE_COMMITTED_AT:-}"
    if [ -z "$SOURCE_SHA" ]; then
        SOURCE_SHA="$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null)" || return 1
    fi
    if [ -z "$SOURCE_COMMITTED_AT" ]; then
        SOURCE_COMMITTED_AT="$(git -C "$PROJECT_ROOT" show -s --format=%cI "$SOURCE_SHA" 2>/dev/null)" || return 1
    fi
}

if ! resolve_identity; then
    echo "Exact source SHA and committed-at timestamp are required" >&2
    exit 1
fi

case "$MODE" in
    check)
        exec "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_currentness.py" check             --source-sha "$SOURCE_SHA"             --source-committed-at "$SOURCE_COMMITTED_AT"
        ;;
    help|-h|--help)
        echo "Usage: $0 [update|full|quick|check]"
        echo "  update/full  regenerate every declared output and emit health evidence"
        echo "  quick        non-release partial generation; health remains failed"
        echo "  check        generate twice in isolated roots and compare exact bytes"
        exit 0
        ;;
    update|full|quick) ;;
    *) echo "Unknown command: $MODE" >&2; exit 1 ;;
esac

RESULTS_FILE="$(mktemp)"
HEALTH_OUTPUT="${SPARK_DOC_HEALTH_OUTPUT:-$SCRIPT_DIR/.health.json}"
STARTED_AT="$("$PYTHON_BIN" -c "from datetime import datetime,timezone; print(datetime.now(timezone.utc).isoformat().replace('+00:00','Z'))")"
FINAL_EXIT=1

write_health() {
    local code="$1"
    "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_currentness.py" write-health         --mode "$MODE"         --results "$RESULTS_FILE"         --output "$HEALTH_OUTPUT"         --source-sha "$SOURCE_SHA"         --source-committed-at "$SOURCE_COMMITTED_AT"         --started-at "$STARTED_AT"         --exit-code "$code"
}

finish() {
    local observed="$?"
    trap - EXIT INT TERM
    local code="$FINAL_EXIT"
    if [ "$observed" -ne 0 ]; then
        code="$observed"
    fi
    if ! write_health "$code"; then
        code=1
    fi
    rm -f "$RESULTS_FILE"
    exit "$code"
}
trap finish EXIT
trap 'FINAL_EXIT=130; exit 130' INT TERM

record() {
    local id="$1"
    local status="$2"
    local message="$3"
    message="${message//$'\t'/ }"
    message="${message//$'\r'/ }"
    message="${message//$'\n'/ }"
    printf '%s\t%s\t%s\n' "$id" "$status" "$message" >> "$RESULTS_FILE"
}

run_generator() {
    local id="$1"
    local name="$2"
    local script="$3"
    local generator_mode="$4"
    local full_path="$SCRIPT_DIR/$script"
    echo "[DOCS] $name"
    if [ ! -f "$full_path" ]; then
        record "$id" "missing" "required generator script is missing: $script"
        return 1
    fi
    local code=0
    if command -v timeout >/dev/null 2>&1; then
        timeout --signal=TERM 300s bash "$full_path" "$generator_mode" || code=$?
    else
        bash "$full_path" "$generator_mode" || code=$?
    fi
    if [ "$code" -eq 0 ]; then
        record "$id" "current" "generator completed successfully"
        return 0
    fi
    record "$id" "failed" "generator exited $code"
    return 1
}

failures=0
run_generator "wiki-sync" "Wiki Sync" "sync-wiki.sh" "sync" || failures=$((failures + 1))

if [ "$MODE" = "quick" ]; then
    record "api-docs" "skipped" "quick mode is not release evidence"
    record "symbol-indexes" "skipped" "quick mode is not release evidence"
    record "file-tree" "skipped" "quick mode is not release evidence"
    record "class-hierarchy" "skipped" "quick mode is not release evidence"
    record "architecture-flowchart" "skipped" "quick mode is not release evidence"
    failures=$((failures + 5))
else
    run_generator "api-docs" "API Docs" "generate-api-docs.sh" "generate" || failures=$((failures + 1))
    run_generator "symbol-indexes" "Symbol Indexes" "generate-symbol-index.sh" "generate" || failures=$((failures + 1))
    run_generator "file-tree" "File Tree" "generate-file-tree.sh" "generate" || failures=$((failures + 1))
    run_generator "class-hierarchy" "Class Hierarchy" "generate-class-hierarchy.sh" "generate" || failures=$((failures + 1))
    run_generator "architecture-flowchart" "Architecture Flowchart" "generate-flowchart.sh" "generate" || failures=$((failures + 1))
fi

run_generator "codebase-statistics" "Codebase Statistics" "update-codebase-stats.sh" "generate" || failures=$((failures + 1))
run_generator "readme-badges" "README Badges" "update-readme-badges.sh" "update" || failures=$((failures + 1))
run_generator "ai-context" "AI Context" "update-context.sh" "update" || failures=$((failures + 1))

if [ "$failures" -eq 0 ]; then
    FINAL_EXIT=0
    echo "[DOCS] all declared generators completed"
else
    FINAL_EXIT=1
    echo "[DOCS] $failures declared generator(s) failed or were skipped" >&2
fi
exit "$FINAL_EXIT"
