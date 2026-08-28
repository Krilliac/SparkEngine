#!/usr/bin/env bash
set -uo pipefail

usage() {
    echo "Usage: $0 [--runtime-log-prefix PREFIX] <report-path> <console-path> -- <test-command> [args...]" >&2
}

# Parse optional flags before positional arguments.
runtime_log_prefix=""
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --runtime-log-prefix)
            if [[ "$#" -lt 2 ]]; then
                usage
                exit 2
            fi
            runtime_log_prefix="$2"
            shift 2
            ;;
        -*)
            echo "error: unknown flag: $1" >&2
            usage
            exit 2
            ;;
        *)
            break
            ;;
    esac
done

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

# ── Sanitizer evidence verification ──────────────────────────────────────────
# Detect sanitizer error signatures in console output.
sanitizer_signatures="no"
if grep -qE \
    'ERROR:\s*(Address|Leak|Thread|Memory)Sanitizer:|WARNING:\s*(Thread|Memory)Sanitizer:|DEADLYSIGNAL|runtime error:' \
    "$console_path" 2>/dev/null; then
    sanitizer_signatures="yes"
fi

# Check for runtime log files at the prefix (e.g. build/asan-runtime.12345).
runtime_log_count=0
if [[ -n "$runtime_log_prefix" ]]; then
    for _f in "${runtime_log_prefix}".*; do
        [[ -f "$_f" ]] && ((runtime_log_count++)) || true
    done
fi

# Classify the evidence:
#   clean                — exit 0, no sanitizer signatures, no runtime logs
#   finding              — non-zero exit with sanitizer evidence present
#   infrastructure-failure — non-zero exit with NO sanitizer evidence
#   evidence-gap         — exit 0 but sanitizer runtime logs or signatures exist
evidence_classification="unknown"
if [[ "$effective_status" -eq 0 ]]; then
    if [[ "$sanitizer_signatures" == "yes" || "$runtime_log_count" -gt 0 ]]; then
        evidence_classification="evidence-gap"
        echo "::error::Sanitizer evidence found but process exited cleanly — overriding to failure"
        effective_status=1
    else
        evidence_classification="clean"
    fi
else
    if [[ "$sanitizer_signatures" == "yes" || "$runtime_log_count" -gt 0 ]]; then
        evidence_classification="finding"
    else
        evidence_classification="infrastructure-failure"
        echo "::warning::Non-zero exit ($effective_status) with no sanitizer signatures — possible infrastructure failure"
    fi
fi

# ── Process footer ───────────────────────────────────────────────────────────
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
        echo "sanitizer_signatures=$sanitizer_signatures"
        echo "runtime_log_count=$runtime_log_count"
        echo "evidence_classification=$evidence_classification"
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
