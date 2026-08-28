#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="${SPARK_DOC_API_OUTPUT_DIR:-$SCRIPT_DIR/api}"

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

generate() {
    "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_contract.py" generate-api --output "$OUTPUT_DIR"
}

check() {
    local temp_root
    temp_root="$(mktemp -d)"
    trap 'rm -rf "$temp_root"' RETURN
    SPARK_DOC_API_OUTPUT_DIR="$temp_root/a" bash "$0" generate
    SPARK_DOC_API_OUTPUT_DIR="$temp_root/b" bash "$0" generate
    "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_contract.py" validate-api-manifest --api-dir "$temp_root/a"
    "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_contract.py" validate-api-manifest --api-dir "$temp_root/b"
    if ! diff --recursive --brief --no-dereference "$temp_root/a" "$temp_root/b"; then
        echo "API documentation generation is nondeterministic" >&2
        return 1
    fi
    echo "API documentation generation is deterministic and manifest-complete"
}

case "${1:-generate}" in
    generate|full) generate ;;
    check) check ;;
    status) "$PYTHON_BIN" "$PROJECT_ROOT/tools/docs_contract.py" validate-api-manifest --api-dir "$OUTPUT_DIR" ;;
    help|-h|--help) echo "Usage: $0 [generate|check|status]" ;;
    *) echo "Unknown command: $1" >&2; exit 1 ;;
esac
