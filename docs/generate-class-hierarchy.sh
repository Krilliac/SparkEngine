#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_FILE="${SPARK_CLASS_HIERARCHY_OUTPUT:-$PROJECT_ROOT/wiki/reference/Class-Hierarchy.md}"
PYTHON_BIN="${PYTHON:-python3}"

generate() {
    "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_contract.py" generate-hierarchy --output "$OUTPUT_FILE"
}

check() {
    local generated
    generated="$(mktemp)"
    trap 'rm -f "$generated"' RETURN
    SPARK_CLASS_HIERARCHY_OUTPUT="$generated" bash "$0" generate
    if ! cmp -s "$generated" "$OUTPUT_FILE"; then
        echo "$OUTPUT_FILE is stale or missing" >&2
        return 1
    fi
}

case "${1:-generate}" in
    generate|full|update) generate ;;
    check) check ;;
    help|-h|--help) echo "Usage: $0 [generate|check]" ;;
    *) echo "Unknown command: $1" >&2; exit 1 ;;
esac
