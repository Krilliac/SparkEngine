#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERIFIER="$SCRIPT_DIR/verify-sanitizer-evidence.py"

usage() {
    cat >&2 <<'EOF'
Usage: run-sanitizer-tests.sh \
  --evidence-root DIR --sanitizer asan|tsan|msan \
  --expected-sha SHA --run-id ID --run-attempt N --job JOB \
  --expected-selector all --minimum-tests N --timeout-seconds N \
  --runtime-env ASAN_OPTIONS|TSAN_OPTIONS|MSAN_OPTIONS \
  -- <test-command> [args...]
EOF
}

die_usage() {
    echo "error: $1" >&2
    usage
    exit 2
}

evidence_root=""
sanitizer=""
expected_sha=""
run_id=""
run_attempt=""
job=""
expected_selector=""
minimum_tests=""
timeout_seconds=""
runtime_env=""

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --evidence-root|--sanitizer|--expected-sha|--run-id|--run-attempt|--job|--expected-selector|--minimum-tests|--timeout-seconds|--runtime-env)
            [[ "$#" -ge 2 ]] || die_usage "missing value for $1"
            option="$1"
            value="$2"
            case "$option" in
                --evidence-root) evidence_root="$value" ;;
                --sanitizer) sanitizer="$value" ;;
                --expected-sha) expected_sha="$value" ;;
                --run-id) run_id="$value" ;;
                --run-attempt) run_attempt="$value" ;;
                --job) job="$value" ;;
                --expected-selector) expected_selector="$value" ;;
                --minimum-tests) minimum_tests="$value" ;;
                --timeout-seconds) timeout_seconds="$value" ;;
                --runtime-env) runtime_env="$value" ;;
            esac
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*) die_usage "unknown flag: $1" ;;
        *) die_usage "expected -- before the test command" ;;
    esac
done

[[ "$#" -gt 0 ]] || die_usage "test command is missing"
command=("$@")

[[ -n "$evidence_root" ]] || die_usage "--evidence-root is required"
[[ "$sanitizer" =~ ^(asan|tsan|msan)$ ]] || die_usage "unsupported sanitizer: $sanitizer"
[[ "$expected_sha" =~ ^[0-9a-f]{40}$ ]] || die_usage "--expected-sha must be a lowercase 40-character SHA"
[[ "$run_id" =~ ^[1-9][0-9]*$ ]] || die_usage "--run-id must be a positive integer"
[[ "$run_attempt" =~ ^[1-9][0-9]*$ ]] || die_usage "--run-attempt must be a positive integer"
[[ "$job" =~ ^[A-Za-z0-9._-]+$ ]] || die_usage "--job contains unsupported characters"
(( ${#run_id} <= 20 )) || die_usage "--run-id is too long"
(( ${#run_attempt} <= 10 )) || die_usage "--run-attempt is too long"
(( ${#job} <= 64 )) || die_usage "--job is too long"
[[ "$job" == "build-linux-$sanitizer" ]] || die_usage "--job does not match --sanitizer"
[[ "$expected_selector" == "all" ]] || die_usage "only the exact 'all' selector is supported"
[[ "$minimum_tests" =~ ^[1-9][0-9]*$ ]] || die_usage "--minimum-tests must be a positive integer"
[[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]] || die_usage "--timeout-seconds must be a positive integer"
(( timeout_seconds <= 7200 )) || die_usage "--timeout-seconds exceeds the 7200 second limit"
(( minimum_tests <= 10000 )) || die_usage "--minimum-tests exceeds the verifier ceiling"

case "$sanitizer:$runtime_env" in
    asan:ASAN_OPTIONS|tsan:TSAN_OPTIONS|msan:MSAN_OPTIONS) ;;
    *) die_usage "--runtime-env does not match --sanitizer" ;;
esac

[[ -f "$VERIFIER" && ! -L "$VERIFIER" ]] || die_usage "sanitizer verifier is missing or unsafe"
PYTHON_BIN="python3"
if ! command -v "$PYTHON_BIN" >/dev/null 2>&1 || ! "$PYTHON_BIN" -c 'import sys' >/dev/null 2>&1; then
    PYTHON_BIN="python"
fi
command -v "$PYTHON_BIN" >/dev/null 2>&1 || die_usage "Python 3 is required"
"$PYTHON_BIN" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 9) else 1)' \
    >/dev/null 2>&1 || die_usage "Python 3.9 or newer is required"
command -v timeout >/dev/null 2>&1 || die_usage "GNU timeout is required"

# Exact provenance must agree with GitHub's immutable context when present.
[[ -z "${GITHUB_SHA:-}" || "$GITHUB_SHA" == "$expected_sha" ]] || die_usage "--expected-sha disagrees with GITHUB_SHA"
[[ -z "${GITHUB_RUN_ID:-}" || "$GITHUB_RUN_ID" == "$run_id" ]] || die_usage "--run-id disagrees with GITHUB_RUN_ID"
[[ -z "${GITHUB_RUN_ATTEMPT:-}" || "$GITHUB_RUN_ATTEMPT" == "$run_attempt" ]] || die_usage "--run-attempt disagrees with GITHUB_RUN_ATTEMPT"
[[ -z "${GITHUB_JOB:-}" || "$GITHUB_JOB" == "$job" ]] || die_usage "--job disagrees with GITHUB_JOB"

# The all-tests selector is only trustworthy if no hidden environment filter is
# active. TestMain emits its final selected count to JUnit; the verifier binds
# that count to both terminal text streams.
for selector_var in SPARK_TEST_LIMIT SPARK_TEST_FILE SPARK_TEST_NAME SPARK_TEST_EXCLUDE; do
    [[ ! -v "$selector_var" ]] || die_usage "$selector_var must be unset for selector=all"
done

warn_is_error_count=0
shuffle_count=0
for ((index = 0; index < ${#command[@]}; ++index)); do
    case "${command[$index]}" in
        --output-file|--output-file=*|--junit-xml|--junit-xml=*|--list-warnings|--list-tests|--help)
            die_usage "the runner owns completion-evidence arguments"
            ;;
        --warn-is-error) warn_is_error_count=$((warn_is_error_count + 1)) ;;
        --warn-is-error=*) die_usage "--warn-is-error does not accept a value" ;;
        --shuffle)
            (( index + 1 < ${#command[@]} )) || die_usage "--shuffle is missing its seed"
            [[ "${command[$((index + 1))]}" == "123" ]] || die_usage "sanitizer lanes require the exact shuffle seed 123"
            shuffle_count=$((shuffle_count + 1))
            index=$((index + 1))
            ;;
        --shuffle=*) die_usage "--shuffle must be passed exactly as --shuffle 123" ;;
        --retry|--retry=*|--retries|--retries=*|--retry-*)
            die_usage "retry flags are forbidden in required sanitizer lanes"
            ;;
    esac
done
if [[ "$sanitizer" == "asan" || "$sanitizer" == "tsan" ]]; then
    (( warn_is_error_count == 1 )) || die_usage "required sanitizer lanes must pass --warn-is-error exactly once"
elif (( warn_is_error_count > 1 )); then
    die_usage "--warn-is-error may appear at most once"
fi
(( shuffle_count == 1 )) || die_usage "sanitizer lanes must use exactly one deterministic --shuffle 123"

runtime_options=""
if [[ -v "$runtime_env" ]]; then
    runtime_options="${!runtime_env}"
fi
if [[ ":$runtime_options:" == *":log_path="* ]]; then
    die_usage "$runtime_env already contains an untrusted log_path"
fi

[[ "$evidence_root" == /* ]] || die_usage "--evidence-root must be absolute"
[[ -d "$evidence_root" && ! -L "$evidence_root" ]] || die_usage "--evidence-root must be a regular non-symlink directory"
"$PYTHON_BIN" -c 'import os, stat, sys; p=sys.argv[1]; s=os.lstat(p); j=getattr(os.path, "isjunction", None); raise SystemExit(1 if stat.S_ISLNK(s.st_mode) or (j and j(p)) else 0)' \
    "$evidence_root" >/dev/null 2>&1 || die_usage "--evidence-root must not be a junction or reparse link"
evidence_root="$(cd "$evidence_root" && pwd -P)"
evidence_dir="$evidence_root/spark-sanitizer-$sanitizer-$expected_sha-$run_id-$run_attempt-$job"
[[ ! -e "$evidence_dir" && ! -L "$evidence_dir" ]] || die_usage "fresh evidence directory already exists"

umask 077
mkdir -m 700 "$evidence_dir" || die_usage "cannot create private evidence directory"
runtime_dir="$evidence_dir/runtime"
mkdir -m 700 "$runtime_dir" || die_usage "cannot create private runtime directory"

report_path="$evidence_dir/report.txt"
console_path="$evidence_dir/console.txt"
junit_path="$evidence_dir/junit.xml"
metadata_path="$evidence_dir/metadata.json"
footer_path="$evidence_dir/process-footer.txt"
runtime_prefix="$runtime_dir/sanitizer"
timeout_marker="$evidence_dir/.wrapper-timeout"
capture_overflow_marker="$evidence_dir/.capture-overflow"

for path in "$report_path" "$console_path" "$junit_path" "$metadata_path" "$footer_path" "$timeout_marker" "$capture_overflow_marker"; do
    [[ ! -e "$path" && ! -L "$path" ]] || die_usage "preexisting evidence path: $path"
done

# Create the footer once with an exclusive redirection and retain the opened
# descriptor.  Later signal/EXIT handlers never reopen an attacker-swappable path.
set -C
if ! exec {footer_fd}> "$footer_path"; then
    set +C
    die_usage "cannot create the private process footer"
fi
set +C

export "$runtime_env=${runtime_options:+$runtime_options:}log_path=$runtime_prefix"
started_ns="$("$PYTHON_BIN" -c 'import time; print(time.time_ns())')" || die_usage "cannot capture start time"
command_sha256="$("$PYTHON_BIN" -c 'import hashlib, os, sys; print(hashlib.sha256(b"\0".join(os.fsencode(value) for value in sys.argv[1:])).hexdigest())' "${command[@]}")" || die_usage "cannot hash command provenance"
timeout_token="$("$PYTHON_BIN" -c 'import secrets; print(secrets.token_hex(32))')" || die_usage "cannot create private timeout marker"
capture_token="$("$PYTHON_BIN" -c 'import secrets; print(secrets.token_hex(32))')" || die_usage "cannot create private capture marker"

process_status=125
capture_status=125
effective_status=70
classification="not-started"
signal_received="none"
footer_written=0

append_footer() {
    (( footer_written == 0 )) || return 0
    footer_written=1
    local completed_utc
    local metadata_sha256="unavailable"
    completed_utc="$(date -u +'%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || printf 'unavailable')"
    if [[ -f "$metadata_path" && ! -L "$metadata_path" ]]; then
        metadata_sha256="$("$PYTHON_BIN" -c 'import hashlib, sys; print(hashlib.sha256(open(sys.argv[1], "rb").read()).hexdigest())' "$metadata_path" 2>/dev/null || printf 'unavailable')"
    fi
    {
        echo "=== SparkEngine CI sanitizer process result ==="
        echo "commit_sha=$expected_sha"
        echo "run_id=$run_id"
        echo "run_attempt=$run_attempt"
        echo "job=$job"
        echo "sanitizer=$sanitizer"
        echo "expected_selector=$expected_selector"
        echo "minimum_tests=$minimum_tests"
        echo "timeout_seconds=$timeout_seconds"
        echo "test_exit_code=$process_status"
        echo "capture_exit_code=$capture_status"
        echo "effective_exit_code=$effective_status"
        echo "signal_received=$signal_received"
        echo "evidence_classification=$classification"
        echo "metadata_file=metadata.json"
        echo "metadata_sha256=$metadata_sha256"
        echo "command_sha256=$command_sha256"
        echo "completed_utc=$completed_utc"
    } >&"$footer_fd"
    exec {footer_fd}>&-

}

on_signal() {
    signal_received="$1"
    effective_status="$2"
    classification="interrupted"
    append_footer
    trap - "$1"
    exit "$2"
}

on_exit() {
    local status="$1"
    if [[ "$classification" == "not-started" ]]; then
        effective_status="$status"
        classification="wrapper-exit"
    fi
    append_footer
}

trap 'on_signal TERM 143' TERM
trap 'on_signal INT 130' INT
trap 'on_exit $?' EXIT

set +e
SPARK_WRAPPER_TIMEOUT_MARKER="$timeout_marker" \
SPARK_WRAPPER_TIMEOUT_TOKEN="$timeout_token" \
timeout --signal=TERM --kill-after=15s "${timeout_seconds}s" \
    bash -c '
        set -uo pipefail
        marker_path="${SPARK_WRAPPER_TIMEOUT_MARKER:?}"
        marker_token="${SPARK_WRAPPER_TIMEOUT_TOKEN:?}"
        unset SPARK_WRAPPER_TIMEOUT_MARKER SPARK_WRAPPER_TIMEOUT_TOKEN
        on_wrapper_timeout() {
            umask 077
            set -C
            printf "timeout:%s\n" "$marker_token" > "$marker_path" 2>/dev/null || true
            set +C
        }
        trap on_wrapper_timeout TERM
        # Bash reports -f in 1024-byte blocks. Keep the hard limit unchanged so
        # tests that deliberately construct oversized sparse fixtures can raise
        # and restore their own soft limit. Cooperative child output defaults
        # to a 16 MiB write cap; the bounded verifier remains authoritative.
        ulimit -S -f 16384
        "$@" &
        child_pid=$!
        wait "$child_pid"
        child_status=$?
        trap - TERM
        exit "$child_status"
    ' spark-timeout-wrapper \
    "${command[@]}" \
    --output-file "$report_path" \
    --junit-xml "$junit_path" \
    2>&1 | "$PYTHON_BIN" "$VERIFIER" capture \
        --output "$console_path" \
        --overflow-marker "$capture_overflow_marker" \
        --marker-token "$capture_token"
pipeline_status=("${PIPESTATUS[@]}")
set -e

process_status="${pipeline_status[0]}"
capture_status="${pipeline_status[1]}"

# log_path keeps the sanitizer stream out of the console/JUnit evidence being
# verified, which also kept every finding out of the job log: the report only
# existed inside a multi-GB artifact.  Echo a bounded excerpt for a non-clean
# classification.  The runtime report is untrusted text, so workflow commands
# are stopped around it instead of mangling the "::" in demangled C++ frames.
RUNTIME_EXCERPT_LINES=200
RUNTIME_EXCERPT_BYTES=65536

emit_runtime_excerpt() {
    local runtime_file
    local token
    local excerpt_files=()
    for runtime_file in "$runtime_dir"/sanitizer.*; do
        [[ -f "$runtime_file" && ! -L "$runtime_file" ]] && excerpt_files+=("$runtime_file")
    done
    if (( ${#excerpt_files[@]} == 0 )); then
        echo "Sanitizer runtime reports: none were written to the private runtime directory."
        return 0
    fi
    token="$("$PYTHON_BIN" -c 'import secrets; print(secrets.token_hex(16))' 2>/dev/null)"
    echo "::group::Sanitizer runtime findings ($sanitizer, first $RUNTIME_EXCERPT_LINES lines per report)"
    if [[ -n "$token" ]]; then
        echo "::stop-commands::$token"
    fi
    {
        for runtime_file in "${excerpt_files[@]}"; do
            printf -- '--- %s ---\n' "${runtime_file##*/}"
            head -n "$RUNTIME_EXCERPT_LINES" -- "$runtime_file"
        done
    } 2>/dev/null | head -c "$RUNTIME_EXCERPT_BYTES"
    printf '\n'
    if [[ -n "$token" ]]; then
        echo "::$token::"
    fi
    echo "::endgroup::"
    echo "Full sanitizer reports remain in the sanitizer-report-$sanitizer artifact."
}

scan() {
    local pattern="$1"
    local include_runtime="${2:-0}"
    local scan_files=()
    [[ -f "$console_path" && ! -L "$console_path" ]] && scan_files+=("$console_path")
    [[ -f "$report_path" && ! -L "$report_path" ]] && scan_files+=("$report_path")
    if [[ "$include_runtime" == "1" ]]; then
        local runtime_file
        for runtime_file in "$runtime_dir"/sanitizer.*; do
            [[ -f "$runtime_file" && ! -L "$runtime_file" ]] && scan_files+=("$runtime_file")
        done
    fi
    (( ${#scan_files[@]} > 0 )) || {
        printf '2'
        return 0
    }
    set +e
    grep -qE "$pattern" "${scan_files[@]}" >/dev/null 2>&1
    local status="$?"
    set -e
    printf '%s' "$status"
}

signature_scan_status="$(scan 'ERROR:[[:space:]]*(Address|Leak|Thread|Memory)Sanitizer:|WARNING:[[:space:]]*(Thread|Memory)Sanitizer:|SUMMARY:[[:space:]]*(Address|Leak|Thread|Memory)Sanitizer:|AddressSanitizer:DEADLYSIGNAL|runtime error:' 1)"
warning_scan_status="$(scan '^\[[[:space:]]*WARN[[:space:]]*\]|Known flaky|::warning title=Flaky test:')"
failure_scan_status="$(scan '^\[[[:space:]]*FAILED[[:space:]]*\]|^Tests:.*[1-9][0-9]* failed|^Assertions:.*[1-9][0-9]* failed')"
crash_scan_status="$(scan 'Segmentation fault|core dumped|AddressSanitizer:DEADLYSIGNAL|terminate called|uncaught exception|(^|[[:space:]])Aborted([[:space:]]|$)')"
infrastructure_scan_status="$(scan 'command not found|No such file or directory|cannot execute|Permission denied|failed to start process')"

set +e
classification="$("$PYTHON_BIN" "$VERIFIER" \
    --evidence-dir "$evidence_dir" \
    --console "$console_path" \
    --report "$report_path" \
    --junit "$junit_path" \
    --runtime-dir "$runtime_dir" \
    --metadata "$metadata_path" \
    --sanitizer "$sanitizer" \
    --expected-sha "$expected_sha" \
    --run-id "$run_id" \
    --run-attempt "$run_attempt" \
    --job "$job" \
    --expected-selector "$expected_selector" \
    --minimum-tests "$minimum_tests" \
    --process-exit "$process_status" \
    --capture-exit "$capture_status" \
    --signature-scan-status "$signature_scan_status" \
    --warning-scan-status "$warning_scan_status" \
    --failure-scan-status "$failure_scan_status" \
    --crash-scan-status "$crash_scan_status" \
    --infrastructure-scan-status "$infrastructure_scan_status" \
    --started-ns "$started_ns" \
    --timeout-seconds "$timeout_seconds" \
    --command-sha256 "$command_sha256" \
    --timeout-marker "$timeout_marker" \
    --capture-overflow-marker "$capture_overflow_marker" \
    --timeout-token "$timeout_token" \
    --capture-token "$capture_token")"
effective_status="$?"
set -e

[[ "$classification" =~ ^[a-z-]+$ ]] || {
    echo "::error::Sanitizer verifier returned an invalid classification"
    classification="verification-failure"
    effective_status="${process_status:-70}"
    [[ "$effective_status" -ne 0 ]] || effective_status=70
}

if [[ "$classification" != "clean" ]]; then
    emit_runtime_excerpt
    echo "::error::Sanitizer evidence classification: $classification"
fi
if [[ "$process_status" -ne 0 ]]; then
    echo "::error::Sanitizer test process exited with status $process_status"
fi
if [[ "$capture_status" -ne 0 ]]; then
    echo "::error::Sanitizer console capture exited with status $capture_status"
fi

append_footer
trap - EXIT TERM INT
exit "$effective_status"
