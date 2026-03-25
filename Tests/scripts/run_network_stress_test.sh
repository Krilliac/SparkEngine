#!/usr/bin/env bash
# run_network_stress_test.sh — End-to-end networking test:
#   1. Start the headless MMO server
#   2. Run the live network stress test
#   3. Stop the server
#   4. Report results
#
# Usage:
#   ./Tests/scripts/run_network_stress_test.sh
#
# Exit code 0 = all tests passed, non-zero = failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== SparkEngine Network Stress Test Suite ==="
echo ""

# ---- 1. Unit tests (fast) -----------------------------------------------
echo "[Phase 1] Running unit tests..."
SPARK_TESTS="${SCRIPT_DIR}/../../build/bin/SparkTests"
if [[ -x "$SPARK_TESTS" ]]; then
    if "$SPARK_TESTS" 2>&1 | tail -3; then
        echo "[Phase 1] PASSED"
    else
        echo "[Phase 1] FAILED — unit tests have failures"
        exit 1
    fi
else
    echo "[Phase 1] SKIPPED — SparkTests not found (build first)"
fi

echo ""

# ---- 2. Start headless server -------------------------------------------
echo "[Phase 2] Starting headless MMO server..."
bash "$SCRIPT_DIR/run_mmo_server.sh" stop  2>/dev/null || true
bash "$SCRIPT_DIR/run_mmo_server.sh" start

echo ""

# ---- 3. Run live network tests ------------------------------------------
echo "[Phase 3] Running live network stress test..."
rc=0
python3 "$SCRIPT_DIR/test_mmo_live.py" || rc=$?

echo ""

# ---- 4. Stop server ------------------------------------------------------
echo "[Phase 4] Stopping server..."
bash "$SCRIPT_DIR/run_mmo_server.sh" stop

echo ""

# ---- Result --------------------------------------------------------------
if [[ $rc -eq 0 ]]; then
    echo "=== ALL PHASES PASSED ==="
else
    echo "=== LIVE TEST PHASE FAILED (exit $rc) ==="
fi

exit $rc
