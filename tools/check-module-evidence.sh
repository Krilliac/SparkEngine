#!/bin/bash
# Validate the module evidence manifest against real CMake targets and source.
# Part of the RDY-010 control plane — fail-closed.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" &>/dev/null; then
    PYTHON="python"
fi

echo "Validating module evidence manifest..."

"$PYTHON" "$SCRIPT_DIR/module-evidence/validate_manifest.py" \
    --manifest "$SCRIPT_DIR/module-evidence/manifest.json" \
    --repo-root "$PROJECT_ROOT"

echo "Running adversarial tests..."

"$PYTHON" -m pytest "$PROJECT_ROOT/Tests/Tools/test_module_evidence.py" \
    -v --tb=short -q 2>&1

echo "Module evidence validation complete."
