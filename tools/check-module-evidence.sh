#!/bin/bash
# Validate the module evidence manifest (RDY-010) — fail-closed.
#
# Local runs have no build artifacts, so this validates the declarative layer
# and runs the adversarial suite. It deliberately does NOT claim the release
# gate: proving that a module really builds and really runs its lifecycle
# needs a configure and a run, which happen in the module-evidence CI job.
# Pass --gated (with produced evidence present) to run the real gate locally.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
EVIDENCE_DIR="$SCRIPT_DIR/module-evidence"

PYTHON="${PYTHON:-python3}"
if ! command -v "$PYTHON" &>/dev/null; then
    PYTHON="python"
fi

cd "$PROJECT_ROOT"

GATED=0
if [ "${1:-}" = "--gated" ]; then
    GATED=1
fi

if [ "$GATED" -eq 1 ]; then
    echo "Validating module evidence against produced artifacts..."
    "$PYTHON" "$EVIDENCE_DIR/validate_manifest.py" \
        --manifest "$EVIDENCE_DIR/manifest.json" \
        --repo-root "$PROJECT_ROOT" \
        --allow-declared-gaps "$EVIDENCE_DIR/evidence-gaps.json"
else
    echo "Validating module evidence manifest (declarative layer)..."
    "$PYTHON" "$EVIDENCE_DIR/validate_manifest.py" \
        --manifest "$EVIDENCE_DIR/manifest.json" \
        --repo-root "$PROJECT_ROOT" \
        --policy-only
fi

echo "Running adversarial tests..."
"$PYTHON" -m unittest Tests.Tools.test_module_evidence 2>&1

echo ""
echo "Tracked evidence gaps that keep RDY-010 release-blocking:"
"$PYTHON" - "$EVIDENCE_DIR/evidence-gaps.json" <<'PYEOF'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    gaps = json.load(handle)["gaps"]
for gap in gaps:
    print(f"  - {gap['evidenceType']} (tracked under {gap['trackedUnder']})")
if not gaps:
    print("  none — RDY-010 may be closeable; re-run with --gated to confirm")
PYEOF

echo ""
echo "Module evidence validation complete."
