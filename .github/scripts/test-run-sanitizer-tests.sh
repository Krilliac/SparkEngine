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

passed=0
failed=0
fail() { echo "FAIL: $1" >&2; failed=$((failed + 1)); }
pass() { echo "PASS: $1"; passed=$((passed + 1)); }

# ── 1. Exit propagation: failure ─────────────────────────────────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/failure-report.txt" "$TMP_ROOT/failure-console.txt" -- \
    bash -c 'printf "failure output\n"; exit 23'
failure_status=$?
set -e
if [[ "$failure_status" -eq 23 ]]; then pass "failure exit propagation"; else fail "failure exit propagation"; fi
if grep -Fq "failure output" "$TMP_ROOT/failure-console.txt"; then pass "failure console capture"; else fail "failure console capture"; fi
if grep -Fq "test_exit_code=23" "$TMP_ROOT/failure-report.txt"; then pass "failure report records test exit"; else fail "failure report records test exit"; fi
if grep -Fq "effective_exit_code=23" "$TMP_ROOT/failure-console.txt"; then pass "failure console records effective exit"; else fail "failure console records effective exit"; fi
bash "$EXTRACTOR" "sanitizer-contract" "$TMP_ROOT/failure-summary.json" \
    "$TMP_ROOT/failure-console.txt"
grep -Fq "effective_exit_code=23" "$TMP_ROOT/failure-summary.json" && pass "failure summary captures effective exit" || fail "failure summary captures effective exit"

# ── 2. Exit propagation: partial output ──────────────────────────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/partial-report.txt" "$TMP_ROOT/partial-console.txt" -- \
    bash -c 'printf "structured prefix\n" > "$1"; printf "sanitizer stack\n"; exit 17' \
    _ "$TMP_ROOT/partial-report.txt"
partial_status=$?
set -e
[[ "$partial_status" -eq 17 ]] && pass "partial exit propagation" || fail "partial exit propagation"
grep -Fq "structured prefix" "$TMP_ROOT/partial-report.txt" && pass "partial report preserves structured prefix" || fail "partial report preserves structured prefix"
grep -Fq "test_exit_code=17" "$TMP_ROOT/partial-report.txt" && pass "partial report records test exit" || fail "partial report records test exit"
grep -Fq "sanitizer stack" "$TMP_ROOT/partial-console.txt" && pass "partial console captures output" || fail "partial console captures output"
grep -Fq "test_exit_code=17" "$TMP_ROOT/partial-console.txt" && pass "partial console records test exit" || fail "partial console records test exit"

# ── 3. Exit propagation: signal ──────────────────────────────────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/signal-report.txt" "$TMP_ROOT/signal-console.txt" -- \
    bash -c 'printf "signal output\n"; kill -TERM "$$"'
signal_status=$?
set -e
[[ "$signal_status" -eq 143 ]] && pass "signal exit propagation" || fail "signal exit propagation"
grep -Fq "signal output" "$TMP_ROOT/signal-console.txt" && pass "signal console capture" || fail "signal console capture"
grep -Fq "test_exit_code=143" "$TMP_ROOT/signal-report.txt" && pass "signal report records test exit" || fail "signal report records test exit"
grep -Fq "effective_exit_code=143" "$TMP_ROOT/signal-console.txt" && pass "signal console records effective exit" || fail "signal console records effective exit"

# ── 4. Exit propagation: success ─────────────────────────────────────────────
bash "$RUNNER" "$TMP_ROOT/success-report.txt" "$TMP_ROOT/success-console.txt" -- \
    bash -c 'printf "success output\n"'
grep -Fq "success output" "$TMP_ROOT/success-console.txt" && pass "success console capture" || fail "success console capture"
grep -Fq "test_exit_code=0" "$TMP_ROOT/success-report.txt" && pass "success report records zero exit" || fail "success report records zero exit"
grep -Fq "effective_exit_code=0" "$TMP_ROOT/success-console.txt" && pass "success console records zero effective exit" || fail "success console records zero effective exit"
bash "$EXTRACTOR" "sanitizer-success-contract" "$TMP_ROOT/success-summary.json" \
    "$TMP_ROOT/success-console.txt"
if grep -Fq "effective_exit_code=0" "$TMP_ROOT/success-summary.json"; then
    fail "successful sanitizer footer should not appear in error summary"
else
    pass "successful sanitizer footer excluded from error summary"
fi

# ── 5. Evidence classification: clean ────────────────────────────────────────
grep -Fq "evidence_classification=clean" "$TMP_ROOT/success-report.txt" && pass "clean run classified as clean" || fail "clean run classified as clean"
grep -Fq "sanitizer_signatures=no" "$TMP_ROOT/success-report.txt" && pass "clean run has no sanitizer signatures" || fail "clean run has no sanitizer signatures"
grep -Fq "runtime_log_count=0" "$TMP_ROOT/success-report.txt" && pass "clean run has zero runtime logs" || fail "clean run has zero runtime logs"

# ── 6. Evidence classification: infrastructure-failure ───────────────────────
grep -Fq "evidence_classification=infrastructure-failure" "$TMP_ROOT/failure-report.txt" && \
    pass "non-zero exit without sanitizer evidence classified as infrastructure-failure" || \
    fail "non-zero exit without sanitizer evidence classified as infrastructure-failure"

# ── 7. Evidence classification: finding (ASan signature in output) ───────────
set +e
bash "$RUNNER" "$TMP_ROOT/asan-finding-report.txt" "$TMP_ROOT/asan-finding-console.txt" -- \
    bash -c 'printf "ERROR: AddressSanitizer: heap-buffer-overflow\n"; exit 1'
asan_finding_status=$?
set -e
[[ "$asan_finding_status" -eq 1 ]] && pass "ASan finding exit propagation" || fail "ASan finding exit propagation"
grep -Fq "evidence_classification=finding" "$TMP_ROOT/asan-finding-report.txt" && \
    pass "ASan signature with non-zero exit classified as finding" || \
    fail "ASan signature with non-zero exit classified as finding"
grep -Fq "sanitizer_signatures=yes" "$TMP_ROOT/asan-finding-report.txt" && \
    pass "ASan signature detected" || fail "ASan signature detected"

# ── 8. Evidence classification: finding (TSan signature in output) ───────────
set +e
bash "$RUNNER" "$TMP_ROOT/tsan-finding-report.txt" "$TMP_ROOT/tsan-finding-console.txt" -- \
    bash -c 'printf "WARNING: ThreadSanitizer: data race\n"; exit 66'
tsan_finding_status=$?
set -e
[[ "$tsan_finding_status" -eq 66 ]] && pass "TSan finding exit propagation (exit 66)" || fail "TSan finding exit propagation (exit 66)"
grep -Fq "evidence_classification=finding" "$TMP_ROOT/tsan-finding-report.txt" && \
    pass "TSan signature with exit 66 classified as finding" || \
    fail "TSan signature with exit 66 classified as finding"
grep -Fq "sanitizer_signatures=yes" "$TMP_ROOT/tsan-finding-report.txt" && \
    pass "TSan signature detected" || fail "TSan signature detected"

# ── 9. Evidence classification: finding (UBSan signature) ────────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/ubsan-finding-report.txt" "$TMP_ROOT/ubsan-finding-console.txt" -- \
    bash -c 'printf "test.cpp:42:3: runtime error: signed integer overflow\n"; exit 1'
ubsan_finding_status=$?
set -e
grep -Fq "sanitizer_signatures=yes" "$TMP_ROOT/ubsan-finding-report.txt" && \
    pass "UBSan runtime error signature detected" || fail "UBSan runtime error signature detected"
grep -Fq "evidence_classification=finding" "$TMP_ROOT/ubsan-finding-report.txt" && \
    pass "UBSan with non-zero exit classified as finding" || fail "UBSan with non-zero exit classified as finding"

# ── 10. Evidence classification: finding (LSan signature) ────────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/lsan-finding-report.txt" "$TMP_ROOT/lsan-finding-console.txt" -- \
    bash -c 'printf "ERROR: LeakSanitizer: detected memory leaks\n"; exit 1'
lsan_finding_status=$?
set -e
grep -Fq "sanitizer_signatures=yes" "$TMP_ROOT/lsan-finding-report.txt" && \
    pass "LSan signature detected" || fail "LSan signature detected"

# ── 11. Evidence classification: finding (MSan signature) ────────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/msan-finding-report.txt" "$TMP_ROOT/msan-finding-console.txt" -- \
    bash -c 'printf "WARNING: MemorySanitizer: use-of-uninitialized-value\n"; exit 1'
msan_finding_status=$?
set -e
grep -Fq "sanitizer_signatures=yes" "$TMP_ROOT/msan-finding-report.txt" && \
    pass "MSan signature detected" || fail "MSan signature detected"

# ── 12. Evidence classification: DEADLYSIGNAL ───────────────────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/deadly-report.txt" "$TMP_ROOT/deadly-console.txt" -- \
    bash -c 'printf "AddressSanitizer:DEADLYSIGNAL\n"; exit 1'
deadly_status=$?
set -e
grep -Fq "sanitizer_signatures=yes" "$TMP_ROOT/deadly-report.txt" && \
    pass "DEADLYSIGNAL signature detected" || fail "DEADLYSIGNAL signature detected"

# ── 13. Evidence-gap detection (false green via signatures) ──────────────────
set +e
bash "$RUNNER" "$TMP_ROOT/gap-sig-report.txt" "$TMP_ROOT/gap-sig-console.txt" -- \
    bash -c 'printf "WARNING: ThreadSanitizer: data race\n"; exit 0'
gap_sig_status=$?
set -e
[[ "$gap_sig_status" -ne 0 ]] && pass "evidence-gap overrides exit 0 to failure" || fail "evidence-gap overrides exit 0 to failure"
grep -Fq "evidence_classification=evidence-gap" "$TMP_ROOT/gap-sig-report.txt" && \
    pass "exit 0 with sanitizer signatures classified as evidence-gap" || \
    fail "exit 0 with sanitizer signatures classified as evidence-gap"

# ── 14. Evidence-gap detection (false green via runtime logs) ────────────────
mkdir -p "$TMP_ROOT/log-gap-build"
echo "race report data" > "$TMP_ROOT/log-gap-build/tsan-runtime.12345"
set +e
bash "$RUNNER" \
    --runtime-log-prefix "$TMP_ROOT/log-gap-build/tsan-runtime" \
    "$TMP_ROOT/gap-log-report.txt" "$TMP_ROOT/gap-log-console.txt" -- \
    bash -c 'printf "all tests passed\n"; exit 0'
gap_log_status=$?
set -e
[[ "$gap_log_status" -ne 0 ]] && pass "runtime logs with exit 0 overrides to failure" || fail "runtime logs with exit 0 overrides to failure"
grep -Fq "evidence_classification=evidence-gap" "$TMP_ROOT/gap-log-report.txt" && \
    pass "exit 0 with runtime logs classified as evidence-gap" || \
    fail "exit 0 with runtime logs classified as evidence-gap"
grep -Fq "runtime_log_count=1" "$TMP_ROOT/gap-log-report.txt" && \
    pass "runtime log count is 1" || fail "runtime log count is 1"

# ── 15. Runtime log detection: no logs ───────────────────────────────────────
mkdir -p "$TMP_ROOT/nolog-build"
bash "$RUNNER" \
    --runtime-log-prefix "$TMP_ROOT/nolog-build/asan-runtime" \
    "$TMP_ROOT/nolog-report.txt" "$TMP_ROOT/nolog-console.txt" -- \
    bash -c 'printf "clean\n"; exit 0'
grep -Fq "runtime_log_count=0" "$TMP_ROOT/nolog-report.txt" && \
    pass "no runtime logs yields count 0" || fail "no runtime logs yields count 0"

# ── 16. Runtime log detection: multiple logs ─────────────────────────────────
mkdir -p "$TMP_ROOT/multi-build"
echo "pid 1" > "$TMP_ROOT/multi-build/asan-runtime.111"
echo "pid 2" > "$TMP_ROOT/multi-build/asan-runtime.222"
echo "pid 3" > "$TMP_ROOT/multi-build/asan-runtime.333"
set +e
bash "$RUNNER" \
    --runtime-log-prefix "$TMP_ROOT/multi-build/asan-runtime" \
    "$TMP_ROOT/multi-report.txt" "$TMP_ROOT/multi-console.txt" -- \
    bash -c 'printf "multi\n"; exit 1'
set -e
grep -Fq "runtime_log_count=3" "$TMP_ROOT/multi-report.txt" && \
    pass "multiple runtime logs counted" || fail "multiple runtime logs counted"

# ── 17. --runtime-log-prefix with nonexistent directory ──────────────────────
bash "$RUNNER" \
    --runtime-log-prefix "$TMP_ROOT/nonexistent-dir/asan-runtime" \
    "$TMP_ROOT/nodir-report.txt" "$TMP_ROOT/nodir-console.txt" -- \
    bash -c 'exit 0'
grep -Fq "runtime_log_count=0" "$TMP_ROOT/nodir-report.txt" && \
    pass "nonexistent log prefix directory yields count 0" || fail "nonexistent log prefix directory yields count 0"

# ── 18. Usage error: unknown flag ────────────────────────────────────────────
set +e
bash "$RUNNER" --bogus-flag 2>/dev/null
unknown_flag_status=$?
set -e
[[ "$unknown_flag_status" -eq 2 ]] && pass "unknown flag exits 2" || fail "unknown flag exits 2"

# ── 19. Usage error: missing arguments ───────────────────────────────────────
set +e
bash "$RUNNER" report.txt 2>/dev/null
missing_args_status=$?
set -e
[[ "$missing_args_status" -eq 2 ]] && pass "missing arguments exits 2" || fail "missing arguments exits 2"

# ══════════════════════════════════════════════════════════════════════════════
# Workflow structure contracts
# ══════════════════════════════════════════════════════════════════════════════

# ── 20. Sanitizer runner invocation count ────────────────────────────────────
invocation_count="$(grep -Fc 'bash .github/scripts/run-sanitizer-tests.sh' "$WORKFLOW")"
[[ "$invocation_count" -eq 3 ]] && pass "exactly 3 sanitizer runner invocations" || fail "exactly 3 sanitizer runner invocations (got $invocation_count)"

# ── 21. Console and runtime log path contracts ───────────────────────────────
grep -Fq 'build/asan-console.txt' "$WORKFLOW" && pass "ASan console path present" || fail "ASan console path present"
grep -Fq 'log_path=${{ github.workspace }}/build/asan-runtime' "$WORKFLOW" && pass "ASan log_path present" || fail "ASan log_path present"
[[ "$(grep -Fc 'build/asan-runtime.*' "$WORKFLOW")" -eq 2 ]] && pass "ASan runtime globs (2)" || fail "ASan runtime globs (2)"
grep -Fq 'build/tsan-console.txt' "$WORKFLOW" && pass "TSan console path present" || fail "TSan console path present"
grep -Fq 'log_path=${{ github.workspace }}/build/tsan-runtime' "$WORKFLOW" && pass "TSan log_path present" || fail "TSan log_path present"
[[ "$(grep -Fc 'build/tsan-runtime.*' "$WORKFLOW")" -eq 2 ]] && pass "TSan runtime globs (2)" || fail "TSan runtime globs (2)"
grep -Fq 'build/msan-console.txt' "$WORKFLOW" && pass "MSan console path present" || fail "MSan console path present"
grep -Fq 'if-no-files-found: error' "$WORKFLOW" && pass "if-no-files-found: error enforced" || fail "if-no-files-found: error enforced"

# ── 22. Runtime log prefix passed to runner ──────────────────────────────────
grep -Fq -- '--runtime-log-prefix build/asan-runtime' "$WORKFLOW" && \
    pass "ASan runtime log prefix passed to runner" || fail "ASan runtime log prefix passed to runner"
grep -Fq -- '--runtime-log-prefix build/tsan-runtime' "$WORKFLOW" && \
    pass "TSan runtime log prefix passed to runner" || fail "TSan runtime log prefix passed to runner"

# ── 23. MSan does NOT pass --runtime-log-prefix (no log_path in MSAN_OPTIONS)
if grep -A5 'Run Tests under MSan' "$WORKFLOW" | grep -Fq -- '--runtime-log-prefix'; then
    fail "MSan should not pass --runtime-log-prefix (no log_path in MSAN_OPTIONS)"
else
    pass "MSan correctly omits --runtime-log-prefix"
fi

# ── 24. TestMain.cpp sanitizer support ───────────────────────────────────────
grep -Fq '__SANITIZE_ADDRESS__' "$TEST_MAIN" && pass "TestMain: __SANITIZE_ADDRESS__ guard" || fail "TestMain: __SANITIZE_ADDRESS__ guard"
grep -Fq '__SANITIZE_THREAD__' "$TEST_MAIN" && pass "TestMain: __SANITIZE_THREAD__ guard" || fail "TestMain: __SANITIZE_THREAD__ guard"
grep -Fq '#if !SPARK_TEST_SANITIZER_BUILD' "$TEST_MAIN" && pass "TestMain: sanitizer build guard" || fail "TestMain: sanitizer build guard"
grep -Fq 'std::cout.flush();' "$TEST_MAIN" && pass "TestMain: flush in sanitizer mode" || fail "TestMain: flush in sanitizer mode"

# ── 25. No raw tee bypass of the exit-propagating runner ─────────────────────
if grep -Eq 'SparkTests .*\| *tee +(asan|tsan|msan)-console\.txt' "$WORKFLOW"; then
    fail "sanitizer workflow bypasses the exit-propagating runner"
else
    pass "no sanitizer tee bypass"
fi

# ── 26. Footer contains all required evidence fields ─────────────────────────
for field in test_exit_code tee_exit_code effective_exit_code sanitizer_signatures runtime_log_count evidence_classification completed_utc; do
    grep -Fq "$field=" "$TMP_ROOT/success-report.txt" && pass "footer field: $field" || fail "footer field: $field"
done

# ── 27. ASan/TSan in required-ci-gate needs ──────────────────────────────────
if grep -A20 'required-ci-gate' "$WORKFLOW" | grep -Fq 'build-linux-asan'; then
    pass "ASan in required-ci-gate"
else
    fail "ASan in required-ci-gate"
fi
if grep -A20 'required-ci-gate' "$WORKFLOW" | grep -Fq 'build-linux-tsan'; then
    pass "TSan in required-ci-gate"
else
    fail "TSan in required-ci-gate"
fi

# ── 28. MSan has continue-on-error and is NOT in required-ci-gate ────────────
asan_section=$(sed -n '/build-linux-asan:/,/^  [a-z]/p' "$WORKFLOW")
if echo "$asan_section" | grep -Fq 'continue-on-error'; then
    fail "ASan must NOT have continue-on-error"
else
    pass "ASan does not have continue-on-error"
fi
msan_section=$(sed -n '/build-linux-msan:/,/^  [a-z]/p' "$WORKFLOW")
if echo "$msan_section" | grep -Fq 'continue-on-error: true'; then
    pass "MSan has continue-on-error: true"
else
    fail "MSan has continue-on-error: true"
fi
gate_needs=$(sed -n '/required-ci-gate:/,/runs-on:/p' "$WORKFLOW")
if echo "$gate_needs" | grep -Fq 'build-linux-msan'; then
    fail "MSan must NOT be in required-ci-gate needs"
else
    pass "MSan not in required-ci-gate"
fi

# ── 29. TSan halt_on_error=0 (intentional: find all races, exitcode=66 still fires)
tsan_section=$(sed -n '/build-linux-tsan:/,/^  [a-z]/p' "$WORKFLOW")
if echo "$tsan_section" | grep -Fq 'halt_on_error=0'; then
    pass "TSan halt_on_error=0 (continue past first race)"
else
    fail "TSan halt_on_error=0 expected"
fi

# ── 30. ASan halt_on_error=1 (strict: abort on first error) ─────────────────
if echo "$asan_section" | grep -Fq 'halt_on_error=1'; then
    pass "ASan halt_on_error=1 (strict)"
else
    fail "ASan halt_on_error=1 expected"
fi

# ══════════════════════════════════════════════════════════════════════════════
# Summary
# ══════════════════════════════════════════════════════════════════════════════
echo
echo "═══════════════════════════════════════════════════════"
echo "Sanitizer pipeline tests: $passed passed, $failed failed"
echo "═══════════════════════════════════════════════════════"
if [[ "$failed" -gt 0 ]]; then
    exit 1
fi
echo "Sanitizer pipeline exit-propagation checks passed."
