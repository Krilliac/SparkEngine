#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNNER="$SCRIPT_DIR/run-sanitizer-tests.sh"
EXTRACTOR="$SCRIPT_DIR/extract-errors.sh"
WORKFLOW="$REPO_ROOT/.github/workflows/build.yml"
TEST_MAIN="$REPO_ROOT/Tests/TestMain.cpp"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

set +e
bash "$RUNNER" "$TMP_ROOT/failure-report.txt" "$TMP_ROOT/failure-console.txt" -- \
    bash -c 'printf "failure output\n"; exit 23'
failure_status=$?
set -e
[[ "$failure_status" -eq 23 ]]
grep -Fq "failure output" "$TMP_ROOT/failure-console.txt"
grep -Fq "test_exit_code=23" "$TMP_ROOT/failure-report.txt"
grep -Fq "effective_exit_code=23" "$TMP_ROOT/failure-console.txt"
bash "$EXTRACTOR" "sanitizer-contract" "$TMP_ROOT/failure-summary.json" \
    "$TMP_ROOT/failure-console.txt"
grep -Fq "effective_exit_code=23" "$TMP_ROOT/failure-summary.json"

set +e
bash "$RUNNER" "$TMP_ROOT/partial-report.txt" "$TMP_ROOT/partial-console.txt" -- \
    bash -c 'printf "structured prefix\n" > "$1"; printf "sanitizer stack\n"; exit 17' \
    _ "$TMP_ROOT/partial-report.txt"
partial_status=$?
set -e
[[ "$partial_status" -eq 17 ]]
grep -Fq "structured prefix" "$TMP_ROOT/partial-report.txt"
grep -Fq "test_exit_code=17" "$TMP_ROOT/partial-report.txt"
grep -Fq "sanitizer stack" "$TMP_ROOT/partial-console.txt"
grep -Fq "test_exit_code=17" "$TMP_ROOT/partial-console.txt"

set +e
bash "$RUNNER" "$TMP_ROOT/signal-report.txt" "$TMP_ROOT/signal-console.txt" -- \
    bash -c 'printf "signal output\n"; kill -TERM "$$"'
signal_status=$?
set -e
[[ "$signal_status" -eq 143 ]]
grep -Fq "signal output" "$TMP_ROOT/signal-console.txt"
grep -Fq "test_exit_code=143" "$TMP_ROOT/signal-report.txt"
grep -Fq "effective_exit_code=143" "$TMP_ROOT/signal-console.txt"

bash "$RUNNER" "$TMP_ROOT/success-report.txt" "$TMP_ROOT/success-console.txt" -- \
    bash -c 'printf "success output\n"'
grep -Fq "success output" "$TMP_ROOT/success-console.txt"
grep -Fq "test_exit_code=0" "$TMP_ROOT/success-report.txt"
grep -Fq "effective_exit_code=0" "$TMP_ROOT/success-console.txt"
bash "$EXTRACTOR" "sanitizer-success-contract" "$TMP_ROOT/success-summary.json" \
    "$TMP_ROOT/success-console.txt"
if grep -Fq "effective_exit_code=0" "$TMP_ROOT/success-summary.json"; then
    echo "A successful sanitizer footer was classified as an error" >&2
    exit 1
fi

invocation_count="$(grep -Fc 'bash .github/scripts/run-sanitizer-tests.sh' "$WORKFLOW")"
[[ "$invocation_count" -eq 3 ]]
grep -Fq 'build/asan-console.txt' "$WORKFLOW"
grep -Fq 'log_path=${{ github.workspace }}/build/asan-runtime' "$WORKFLOW"
[[ "$(grep -Fc 'build/asan-runtime.*' "$WORKFLOW")" -eq 2 ]]
grep -Fq 'build/tsan-console.txt' "$WORKFLOW"
grep -Fq 'log_path=${{ github.workspace }}/build/tsan-runtime' "$WORKFLOW"
[[ "$(grep -Fc 'build/tsan-runtime.*' "$WORKFLOW")" -eq 2 ]]
grep -Fq 'build/msan-console.txt' "$WORKFLOW"
grep -Fq 'if-no-files-found: error' "$WORKFLOW"

grep -Fq '__SANITIZE_ADDRESS__' "$TEST_MAIN"
grep -Fq '__SANITIZE_THREAD__' "$TEST_MAIN"
grep -Fq '#if !SPARK_TEST_SANITIZER_BUILD' "$TEST_MAIN"
grep -Fq 'std::cout.flush();' "$TEST_MAIN"

if grep -Eq 'SparkTests .*\| *tee +(asan|tsan|msan)-console\.txt' "$WORKFLOW"; then
    echo "A sanitizer workflow bypasses the exit-propagating runner" >&2
    exit 1
fi

echo "Sanitizer pipeline exit-propagation checks passed."
