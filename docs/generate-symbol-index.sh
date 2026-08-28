#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
API_DIR="${SPARK_DOC_API_DIR:-$SCRIPT_DIR/api}"
OUTPUT_ROOT="${SPARK_SYMBOL_INDEX_OUTPUT_DIR:-$PROJECT_ROOT/wiki}"
PYTHON_BIN="${PYTHON:-python3}"

generate() {
    "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_contract.py" generate-indexes --api-dir "$API_DIR" --output-root "$OUTPUT_ROOT"
}

check() {
    local temp_root
    temp_root="$(mktemp -d)"
    trap 'rm -rf "$temp_root"' RETURN
    SPARK_DOC_API_OUTPUT_DIR="$temp_root/api" bash "$SCRIPT_DIR/generate-api-docs.sh" generate
    SPARK_DOC_API_DIR="$temp_root/api" SPARK_SYMBOL_INDEX_OUTPUT_DIR="$temp_root/wiki" bash "$0" generate
    local failed=0
    local filename
    for filename in Symbol-Index.md Function-Index.md Class-Index.md Enum-Index.md Macro-Index.md; do
        if ! cmp -s "$temp_root/wiki/reference/$filename" "$OUTPUT_ROOT/reference/$filename"; then
            echo "$OUTPUT_ROOT/reference/$filename is stale or missing" >&2
            failed=1
        fi
    done
    return "$failed"
}

case "${1:-generate}" in
    generate|full|update) generate ;;
    check) check ;;
    help|-h|--help) echo "Usage: $0 [generate|check]" ;;
    *) echo "Unknown command: $1" >&2; exit 1 ;;
esac
