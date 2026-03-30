#!/usr/bin/env bash
# extract-errors.sh — Extract unique build/test errors from log files
#
# Usage: extract-errors.sh <job-name> <output-file> [log-files...]
#
# Produces a JSON file with:
#   { "job": "...", "errors": ["..."], "warnings": ["..."], "tests": ["..."] }
#
# Errors are deduplicated within each category and trimmed to essentials.

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

for logfile in "$@"; do
    [[ -f "$logfile" ]] || continue

    while IFS= read -r line; do
        # Strip ANSI color codes
        clean=$(echo "$line" | sed 's/\x1b\[[0-9;]*m//g')

        # Skip empty or very short lines
        [[ ${#clean} -lt 5 ]] && continue

        # Extract the "signature" — file:line + error message, stripping build paths
        sig=$(echo "$clean" | sed -E 's|/home/[^ ]*/||g; s|[A-Z]:\\[^ ]*\\||g; s|/[^ ]*/build/||g')

        # Test failures
        if echo "$clean" | grep -qiE 'FAILED|FAIL:.*test|tests? failed'; then
            [[ -z "${seen_tests[$sig]+x}" ]] || continue
            seen_tests[$sig]=1
            tests+=("$clean")
            continue
        fi

        # Errors (build errors, fatal errors, sanitizer summaries)
        if echo "$clean" | grep -qiE 'error[:\s]|fatal error|SUMMARY:.*Sanitizer|AddressSanitizer|ThreadSanitizer|MemorySanitizer|LeakSanitizer|UndefinedBehavior'; then
            # Skip "0 errors" and "error(s)" summary lines
            echo "$clean" | grep -qiE '0 error|error\(s\) generated|warnings? generated' && continue
            [[ -z "${seen_errors[$sig]+x}" ]] || continue
            seen_errors[$sig]=1
            errors+=("$clean")
            continue
        fi

        # Warnings (only capture if explicitly a warning line — keep count low)
        if echo "$clean" | grep -qiE 'warning[:\s].*\['; then
            # Skip "0 warnings" summary lines
            echo "$clean" | grep -qiE '0 warning|warnings? generated' && continue
            [[ -z "${seen_warnings[$sig]+x}" ]] || continue
            seen_warnings[$sig]=1
            warnings+=("$clean")
            continue
        fi
    done < "$logfile"
done

# Cap at reasonable limits to prevent huge comments
max_errors=30
max_warnings=15
max_tests=20

# Build JSON output using jq if available, else manual
if command -v jq &>/dev/null; then
    jq -n \
        --arg job "$JOB_NAME" \
        --argjson errors "$(printf '%s\n' "${errors[@]:0:$max_errors}" | jq -R . | jq -s .)" \
        --argjson warnings "$(printf '%s\n' "${warnings[@]:0:$max_warnings}" | jq -R . | jq -s .)" \
        --argjson tests "$(printf '%s\n' "${tests[@]:0:$max_tests}" | jq -R . | jq -s .)" \
        '{job: $job, errors: $errors, warnings: $warnings, tests: $tests}' \
        > "$OUTPUT"
else
    # Fallback: simple text format
    {
        echo "JOB:$JOB_NAME"
        echo "---ERRORS---"
        printf '%s\n' "${errors[@]:0:$max_errors}"
        echo "---WARNINGS---"
        printf '%s\n' "${warnings[@]:0:$max_warnings}"
        echo "---TESTS---"
        printf '%s\n' "${tests[@]:0:$max_tests}"
    } > "$OUTPUT"
fi

total=$(( ${#errors[@]} + ${#warnings[@]} + ${#tests[@]} ))
echo "Extracted $total issues (${#errors[@]} errors, ${#warnings[@]} warnings, ${#tests[@]} test failures) from $JOB_NAME"
