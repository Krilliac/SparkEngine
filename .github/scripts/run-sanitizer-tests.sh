#!/usr/bin/env bash
set -uo pipefail

usage() {
    echo "Usage: $0 <report-path> <console-path> -- <test-command> [args...]" >&2
}

if [[ "$#" -lt 4 ]]; then
    usage
    exit 2
fi

report_path="$1"
console_path="$2"
shift 2
if [[ "$1" != "--" ]]; then
    usage
    exit 2
fi
shift
if [[ "$#" -eq 0 ]]; then
    usage
    exit 2
fi

mkdir -p "$(dirname "$report_path")" "$(dirname "$console_path")"
: > "$report_path"
: > "$console_path"

set +e
"$@" 2>&1 | tee "$console_path"
pipeline_status=("${PIPESTATUS[@]}")
set -e

test_status="${pipeline_status[0]}"
tee_status="${pipeline_status[1]}"
effective_status="$test_status"
if [[ "$effective_status" -eq 0 && "$tee_status" -ne 0 ]]; then
    effective_status="$tee_status"
fi

# A crash can interrupt the test framework's structured writer. Preserve the
# complete console stream separately and append an unambiguous process footer
# to both files so uploaded failure evidence always records how execution ended.
append_process_footer() {
    local destination="$1"
    shift
    {
        echo
        echo "=== SparkEngine CI sanitizer process result ==="
        printf "command="
        printf "%q " "$@"
        echo
        echo "test_exit_code=$test_status"
        echo "tee_exit_code=$tee_status"
        echo "effective_exit_code=$effective_status"
        echo "completed_utc=$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
    } >> "$destination"
}

append_process_footer "$report_path" "$@"
if [[ "$console_path" != "$report_path" ]]; then
    append_process_footer "$console_path" "$@"
fi

if [[ "$test_status" -ne 0 ]]; then
    echo "::error::Sanitizer test process exited with status $test_status"
elif [[ "$tee_status" -ne 0 ]]; then
    echo "::error::Sanitizer console capture exited with status $tee_status"
fi

exit "$effective_status"
