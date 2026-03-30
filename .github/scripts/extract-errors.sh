#!/usr/bin/env bash
# extract-errors.sh — Extract build/test errors from CI log files
#
# Usage: extract-errors.sh <job-name> <output-file> [log-files...]
#
# Produces a JSON file with:
#   { "job": "...", "errors": ["..."], "warnings": ["..."], "tests": ["..."] }
#
# The script separates cmake configure logs from build/test logs so that
# configure-time noise (missing deps warnings, feature-check failures)
# doesn't drown out real compile errors.

set -euo pipefail

JOB_NAME="${1:?Usage: extract-errors.sh <job-name> <output-file> [log-files...]}"
OUTPUT="${2:?Usage: extract-errors.sh <job-name> <output-file> [log-files...]}"
shift 2

declare -A seen_errors
declare -A seen_warnings
declare -A seen_tests
errors=()
warnings=()
tests=()

# Lines matching these patterns are CMake configure noise, not real errors
CMAKE_NOISE_PATTERNS=(
    'To make missing deps a hard error'
    'Performing Test .* - Failed'
    'CMake Warning'
    'CMake Deprecation Warning'
    'Check .* - Failed'
    'Could NOT find'
    'MISSING.*dependency'
    'not found.*install'
    'try setting.*_DIR'
    'cmake-configure\.log'
    'Looking for .* - not found'
    'The imported target .* is not known'
)

# Lines matching these are MSVC build summary noise, not real errors
SUMMARY_NOISE_PATTERNS=(
    '0 error'
    'error\(s\) generated'
    'warnings? generated'
    'Build succeeded'
    'Time Elapsed'
    'errors? and [0-9]+ warning'
    'Build FAILED'
)

is_cmake_noise() {
    local line="$1"
    for pat in "${CMAKE_NOISE_PATTERNS[@]}"; do
        if echo "$line" | grep -qiE "$pat"; then
            return 0
        fi
    done
    return 1
}

is_summary_noise() {
    local line="$1"
    for pat in "${SUMMARY_NOISE_PATTERNS[@]}"; do
        if echo "$line" | grep -qiE "$pat"; then
            return 0
        fi
    done
    return 1
}

# Determine if a log file is a configure log (vs build/test log)
is_configure_log() {
    local filename="$1"
    [[ "$filename" == *configure* ]] && return 0
    return 1
}

for logfile in "$@"; do
    [[ -f "$logfile" ]] || continue

    configure_log=false
    is_configure_log "$logfile" && configure_log=true

    # Capture surrounding context for real errors
    prev_lines=()
    line_num=0

    while IFS= read -r line; do
        line_num=$((line_num + 1))

        # Strip ANSI color codes
        clean=$(echo "$line" | sed 's/\x1b\[[0-9;]*m//g')

        # Skip empty or very short lines
        [[ ${#clean} -lt 5 ]] && continue

        # Signature for dedup: strip build paths but keep file:line
        sig=$(echo "$clean" | sed -E 's|/home/runner/work/[^/]*/[^/]*/||g; s|[A-Z]:\\[^ ]*\\SparkEngine\\||g; s|\s+| |g')

        # ── Test failures ────────────────────────────────────────────
        # Match actual test failures, not cmake feature checks
        if echo "$clean" | grep -qiE '\[ *FAIL|\bFAILED\b.*test|tests? failed[^:]|\bTest #[0-9]+.*Failed'; then
            # Skip cmake feature checks (these are NOT test failures)
            if echo "$clean" | grep -qiE 'Performing Test|Check.*- Failed|Looking for'; then
                continue
            fi
            [[ -z "${seen_tests[$sig]+x}" ]] || continue
            seen_tests[$sig]=1
            tests+=("$clean")
            continue
        fi

        # ── Compile/link errors ──────────────────────────────────────
        # GCC/Clang: "file.cpp:123:45: error: ..."
        # MSVC:      "file.cpp(123): error C2039: ..."
        # Linker:    "undefined reference to" / "unresolved external symbol"
        is_error=false
        if echo "$clean" | grep -qE ':\s*error\s*(C[0-9]+)?:'; then
            is_error=true
        elif echo "$clean" | grep -qE ':\s*fatal error'; then
            is_error=true
        elif echo "$clean" | grep -qiE 'undefined reference to|unresolved external symbol|cannot open input file'; then
            is_error=true
        elif echo "$clean" | grep -qiE 'SUMMARY:.*Sanitizer|AddressSanitizer:|ThreadSanitizer:|MemorySanitizer:|LeakSanitizer:|UndefinedBehavior'; then
            is_error=true
        fi

        if $is_error; then
            # Skip cmake configure noise
            if $configure_log && is_cmake_noise "$clean"; then
                continue
            fi
            # Skip build summary lines
            if is_summary_noise "$clean"; then
                continue
            fi
            [[ -z "${seen_errors[$sig]+x}" ]] || continue
            seen_errors[$sig]=1

            # Include context before the error for
            # "In file included from" / "In instantiation of" breadcrumbs
            context=""
            for ctx_line in "${prev_lines[@]}"; do
                if echo "$ctx_line" | grep -qiE 'In file included from|In instantiation of|note:.*required from|In member function'; then
                    context+="$ctx_line → "
                fi
            done
            errors+=("${context}${clean}")
            continue
        fi

        # ── Warnings ─────────────────────────────────────────────────
        # GCC/Clang: "warning: ... [-Wflag]"
        # MSVC:      "warning C4XXX: ..."
        if echo "$clean" | grep -qE ':\s*warning\s*(C[0-9]+|\[-W)'; then
            if $configure_log; then continue; fi
            is_summary_noise "$clean" && continue
            [[ -z "${seen_warnings[$sig]+x}" ]] || continue
            seen_warnings[$sig]=1
            warnings+=("$clean")
            continue
        fi

        # Track last 3 lines for context
        prev_lines+=("$clean")
        if [[ ${#prev_lines[@]} -gt 3 ]]; then
            prev_lines=("${prev_lines[@]:1}")
        fi
    done < "$logfile"
done

# Cap output to prevent huge comments
max_errors=30
max_warnings=15
max_tests=20

# Build JSON output using jq if available, else manual
if command -v jq &>/dev/null; then
    # Handle empty arrays (printf with empty array would emit a blank line)
    err_json="[]"
    warn_json="[]"
    test_json="[]"

    if [[ ${#errors[@]} -gt 0 ]]; then
        err_json=$(printf '%s\n' "${errors[@]:0:$max_errors}" | jq -R . | jq -s .)
    fi
    if [[ ${#warnings[@]} -gt 0 ]]; then
        warn_json=$(printf '%s\n' "${warnings[@]:0:$max_warnings}" | jq -R . | jq -s .)
    fi
    if [[ ${#tests[@]} -gt 0 ]]; then
        test_json=$(printf '%s\n' "${tests[@]:0:$max_tests}" | jq -R . | jq -s .)
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
        [[ ${#errors[@]} -gt 0 ]] && printf '%s\n' "${errors[@]:0:$max_errors}"
        echo "---WARNINGS---"
        [[ ${#warnings[@]} -gt 0 ]] && printf '%s\n' "${warnings[@]:0:$max_warnings}"
        echo "---TESTS---"
        [[ ${#tests[@]} -gt 0 ]] && printf '%s\n' "${tests[@]:0:$max_tests}"
    } > "$OUTPUT"
fi

total=$(( ${#errors[@]} + ${#warnings[@]} + ${#tests[@]} ))
echo "Extracted $total issues (${#errors[@]} errors, ${#warnings[@]} warnings, ${#tests[@]} test failures) from $JOB_NAME"
