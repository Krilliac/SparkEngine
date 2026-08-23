#!/usr/bin/env bash
# extract-errors.sh — Extract build/test errors from CI log files
#
# Usage: extract-errors.sh <job-name> <output-file> [log-files...]
#
# Produces a JSON file with:
#   { "job": "...", "errors": ["..."], "warnings": ["..."], "tests": ["..."] }
#
# The script uses whole-file grep passes instead of per-line loops to avoid
# spawning thousands of subprocesses on large log files.

set -euo pipefail

JOB_NAME="${1:?Usage: extract-errors.sh <job-name> <output-file> [log-files...]}"
OUTPUT="${2:?Usage: extract-errors.sh <job-name> <output-file> [log-files...]}"
shift 2

# Temp files for intermediate results
tmp_errors=$(mktemp)
tmp_warnings=$(mktemp)
tmp_tests=$(mktemp)
trap 'rm -f "$tmp_errors" "$tmp_warnings" "$tmp_tests"' EXIT

# CMake configure noise patterns (combined into one regex)
CMAKE_NOISE='To make missing deps a hard error|Performing Test .* - Failed|CMake Warning|CMake Deprecation Warning|Check .* - Failed|Could NOT find|MISSING.*dependency|not found.*install|try setting.*_DIR|cmake-configure\.log|Looking for .* - not found|The imported target .* is not known'

# Build summary noise
SUMMARY_NOISE='0 error|error\(s\) generated|warnings? generated|Build succeeded|Time Elapsed|errors? and [0-9]+ warning|Build FAILED'

for logfile in "$@"; do
    [[ -f "$logfile" ]] || continue

    # Strip ANSI codes once, work on clean file
    clean_file=$(mktemp)
    sed 's/\x1b\[[0-9;]*m//g' "$logfile" > "$clean_file"

    is_configure=false
    [[ "$logfile" == *configure* ]] && is_configure=true

    # ── Test failures ────────────────────────────────────────────
    # Match test failures but exclude cmake feature checks, "0 tests failed", and warned (flaky) tests
    grep -iE '\[ *FAIL|\bFAILED\b.*test|tests? failed[^:]|\bTest #[0-9]+.*Failed' "$clean_file" 2>/dev/null \
        | grep -viE 'Performing Test|Check.*- Failed|Looking for|0 tests? failed' \
        >> "$tmp_tests" 2>/dev/null || true

    # ── Test crashes (from TestMain.cpp crash handler) ───────────
    # Captures `[ CRASH  ] TestName (EXCEPTION / signal: SIG...)` plus
    # the stack trace lines the crash handler emits below it. These are
    # the lines the CI Error Report groups under "Test Failures" so a
    # segfault stops looking like a silent test timeout.
    grep -E '^\[ *CRASH *\]' "$clean_file" 2>/dev/null \
        >> "$tmp_tests" 2>/dev/null || true
    # Also capture the top few frames of the stack trace so the PR
    # comment reader can see where the crash happened without having to
    # download the full log artifact.
    awk '
        /^\[ *CRASH *\] Stack trace:/ { inTrace = 1; next }
        inTrace && /^  #[0-9]+/ { print; count++; if (count >= 10) { inTrace = 0; count = 0 } next }
        inTrace && /^\[/ { inTrace = 0; count = 0 }
    ' "$clean_file" 2>/dev/null \
        >> "$tmp_tests" 2>/dev/null || true

    # ── Test warnings (known flaky tests) ────────────────────────
    grep -E '\[ *WARN *\]' "$clean_file" 2>/dev/null \
        >> "$tmp_warnings" 2>/dev/null || true

    # ── Compile/link errors ──────────────────────────────────────
    {
        grep -E ':\s*error\s*(C[0-9]+)?:' "$clean_file" 2>/dev/null || true
        grep -E ':\s*fatal error' "$clean_file" 2>/dev/null || true
        grep -iE 'undefined reference to|unresolved external symbol|cannot open input file' "$clean_file" 2>/dev/null || true
        grep -iE 'SUMMARY:.*Sanitizer|AddressSanitizer:|ThreadSanitizer:|MemorySanitizer:|LeakSanitizer:|UndefinedBehavior' "$clean_file" 2>/dev/null || true
    } | {
        # Filter out noise
        if $is_configure; then
            grep -viE "$CMAKE_NOISE"
        else
            cat
        fi
    } | grep -viE "$SUMMARY_NOISE" \
      >> "$tmp_errors" 2>/dev/null || true

    # ── Warnings ─────────────────────────────────────────────────
    if ! $is_configure; then
        grep -E ':\s*warning[ :].*(\[-W|C[0-9]+)' "$clean_file" 2>/dev/null \
            | grep -viE "$SUMMARY_NOISE" \
            >> "$tmp_warnings" 2>/dev/null || true
    fi

    rm -f "$clean_file"
done

# Deduplicate: strip absolute build paths for comparison, then unique
dedup() {
    local file="$1" max="$2"
    sed -E 's|/home/runner/work/[^/]*/[^/]*/||g; s|[A-Z]:\\[^ ]*\\SparkEngine\\||g; s|\s+| |g' "$file" \
        | awk -v max="$max" '!seen[$0]++ && count < max { print; count++ }'
}

max_errors=30
max_warnings=15
max_tests=20

# Build JSON output
if command -v jq &>/dev/null; then
    err_json="[]"
    warn_json="[]"
    test_json="[]"

    err_lines=$(dedup "$tmp_errors" $max_errors)
    warn_lines=$(dedup "$tmp_warnings" $max_warnings)
    test_lines=$(dedup "$tmp_tests" $max_tests)

    if [[ -n "$err_lines" ]]; then
        err_json=$(echo "$err_lines" | jq -R . | jq -s .)
    fi
    if [[ -n "$warn_lines" ]]; then
        warn_json=$(echo "$warn_lines" | jq -R . | jq -s .)
    fi
    if [[ -n "$test_lines" ]]; then
        test_json=$(echo "$test_lines" | jq -R . | jq -s .)
    fi

    jq -n \
        --arg job "$JOB_NAME" \
        --argjson errors "$err_json" \
        --argjson warnings "$warn_json" \
        --argjson tests "$test_json" \
        '{job: $job, errors: $errors, warnings: $warnings, tests: $tests}' \
        > "$OUTPUT"
else
    # Fallback: simple text format
    {
        echo "JOB:$JOB_NAME"
        echo "---ERRORS---"
        dedup "$tmp_errors" $max_errors
        echo "---WARNINGS---"
        dedup "$tmp_warnings" $max_warnings
        echo "---TESTS---"
        dedup "$tmp_tests" $max_tests
    } > "$OUTPUT"
fi

err_count=$(wc -l < "$tmp_errors" 2>/dev/null || echo 0)
warn_count=$(wc -l < "$tmp_warnings" 2>/dev/null || echo 0)
test_count=$(wc -l < "$tmp_tests" 2>/dev/null || echo 0)
total=$((err_count + warn_count + test_count))
echo "Extracted $total issues ($err_count errors, $warn_count warnings, $test_count test failures) from $JOB_NAME"
