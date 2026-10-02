#!/usr/bin/env bash
# SparkEngine shared build-directory lock.
#
# Serializes commands that share one build tree (parallel agents, CI helpers,
# local scripts) behind an advisory flock, with the failure modes of an ad-hoc
# `flock lockfile cmd` designed out:
#
#   * Bounded wait. Acquisition gives up after --timeout seconds (default
#     1800, env SPARK_BUILD_LOCK_TIMEOUT) and exits 75 (EX_TEMPFAIL) instead of
#     queueing forever behind a holder that will never release.
#   * Holder identity. The holder writes pid, host, start time, working
#     directory and command into the lock file, so a waiter (or --status) can
#     say exactly who it is waiting for.
#   * No inherited lock descriptors. The lock fd is held by this wrapper only
#     and is closed in the command it runs, so daemons or detached helpers the
#     command spawns (compiler caches, background waiters) cannot keep the lock
#     alive after the wrapper exits. The kernel releases the lock the moment
#     the wrapper dies, however it dies.
#   * Stuck-holder diagnostics. While waiting, the wrapper periodically reports
#     the recorded holder and every process that actually has the lock file
#     open, and flags a lock held by processes other than the recorded holder
#     (the orphaned-descriptor case). It never kills anything itself.
#
# Usage:
#   tools/build-lock.sh [--lock PATH] [--timeout SEC] [--report-every SEC] -- COMMAND [ARGS...]
#   tools/build-lock.sh [--lock PATH] --status
#
# Examples:
#   tools/build-lock.sh -- cmake --build build/linux-gcc-release --target SparkTests -j4
#   tools/build-lock.sh --timeout 600 -- cmake --preset linux-gcc-release -DBUILD_TESTS=ON
#
# Exit status: the command's own status once it has run; 75 when the lock was
# not acquired in time; 64 for usage errors; 69 when flock(1) is unavailable.
#
# Requires flock(1) (util-linux on Linux; `brew install flock` on macOS).
# Holder discovery reads /proc on Linux and falls back to lsof elsewhere.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

readonly EXIT_USAGE=64
readonly EXIT_UNAVAILABLE=69
readonly EXIT_TIMEOUT=75

lock_path="${SPARK_BUILD_LOCK:-$PROJECT_ROOT/build/.spark-build.lock}"
timeout_seconds="${SPARK_BUILD_LOCK_TIMEOUT:-1800}"
report_every_seconds="${SPARK_BUILD_LOCK_REPORT_EVERY:-60}"
status_only=0

usage()
{
    sed -n '/^# Usage:/,/^# Exit status/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' | sed '$d' >&2
    exit "$EXIT_USAGE"
}

log()
{
    printf '[build-lock] %s\n' "$*" >&2
}

is_non_negative_integer()
{
    [[ "$1" =~ ^[0-9]+$ ]]
}

while [ $# -gt 0 ]; do
    case "$1" in
        --lock)
            [ $# -ge 2 ] || usage
            lock_path="$2"
            shift 2
            ;;
        --timeout)
            [ $# -ge 2 ] || usage
            timeout_seconds="$2"
            shift 2
            ;;
        --report-every)
            [ $# -ge 2 ] || usage
            report_every_seconds="$2"
            shift 2
            ;;
        --status)
            status_only=1
            shift
            ;;
        --)
            shift
            break
            ;;
        -h | --help)
            usage
            ;;
        *)
            log "unknown option: $1 (put the command after --)"
            usage
            ;;
    esac
done

if ! is_non_negative_integer "$timeout_seconds" || ! is_non_negative_integer "$report_every_seconds"; then
    log "--timeout and --report-every take whole seconds"
    exit "$EXIT_USAGE"
fi
if [ "$status_only" -eq 0 ] && [ $# -eq 0 ]; then
    log "no command given"
    usage
fi
if ! command -v flock >/dev/null 2>&1; then
    log "flock(1) is required (util-linux on Linux, 'brew install flock' on macOS)"
    exit "$EXIT_UNAVAILABLE"
fi

mkdir -p "$(dirname "$lock_path")"
if [ -L "$lock_path" ]; then
    # Locking through a symlink would let the lock become a write to an
    # arbitrary redirect target.
    log "refusing to use a symbolic link as the lock file: $lock_path"
    exit "$EXIT_USAGE"
fi
lock_path="$(cd "$(dirname "$lock_path")" && pwd)/$(basename "$lock_path")"

# Open read-write without truncating: another process may hold the lock, and
# its holder record lives in this file.
exec {lock_fd}<>"$lock_path" || {
    log "cannot open lock file: $lock_path"
    exit "$EXIT_USAGE"
}

# Prints the pid of every process with the lock file open, except this one.
lock_file_openers()
{
    if [ -d /proc/self/fd ]; then
        local fd_link pid target
        for fd_link in /proc/[0-9]*/fd/*; do
            target="$(readlink "$fd_link" 2>/dev/null)" || continue
            [ "$target" = "$lock_path" ] || continue
            pid="${fd_link#/proc/}"
            pid="${pid%%/*}"
            [ "$pid" = "$$" ] || echo "$pid"
        done | sort -un
    elif command -v lsof >/dev/null 2>&1; then
        lsof -t -- "$lock_path" 2>/dev/null | grep -vx "$$" | sort -un
    fi
}

describe_pid()
{
    ps -o pid=,ppid=,etime=,args= -p "$1" 2>/dev/null | cut -c1-200
}

report_holder()
{
    local recorded_pid="" line
    if [ -s "$lock_path" ]; then
        log "recorded holder:"
        while IFS= read -r line; do
            log "  $line"
            case "$line" in
                pid=*) recorded_pid="${line#pid=}" ;;
            esac
        done <"$lock_path"
    else
        log "no holder record (holder predates tools/build-lock.sh or did not use it)"
    fi

    local openers
    openers="$(lock_file_openers)"
    if [ -n "$openers" ]; then
        log "processes with the lock file open:"
        local pid description
        for pid in $openers; do
            # A short-lived opener (e.g. a compiler job) may exit between the scan and ps.
            description="$(describe_pid "$pid")"
            [ -n "$description" ] && log "  $description"
        done
    fi

    if [ -n "$recorded_pid" ] && ! kill -0 "$recorded_pid" 2>/dev/null && [ -n "$openers" ]; then
        log "WARNING: recorded holder $recorded_pid is gone but the lock is still held: an orphaned process"
        log "WARNING: inherited the lock descriptor. Inspect the processes above; nothing was killed."
    fi
}

if [ "$status_only" -eq 1 ]; then
    if flock -n "$lock_fd"; then
        flock -u "$lock_fd"
        log "free: $lock_path"
        exit 0
    fi
    log "held: $lock_path"
    report_holder
    exit 1
fi

start_epoch="$(date +%s)"
next_report_epoch=$((start_epoch + report_every_seconds))
reported_wait=0
while ! flock -n "$lock_fd"; do
    now_epoch="$(date +%s)"
    waited=$((now_epoch - start_epoch))
    if [ "$reported_wait" -eq 0 ]; then
        log "waiting for $lock_path (timeout ${timeout_seconds}s)"
        reported_wait=1
    fi
    if [ "$waited" -ge "$timeout_seconds" ]; then
        log "timed out after ${waited}s waiting for $lock_path"
        report_holder
        exit "$EXIT_TIMEOUT"
    fi
    if [ "$report_every_seconds" -gt 0 ] && [ "$now_epoch" -ge "$next_report_epoch" ]; then
        log "still waiting after ${waited}s"
        report_holder
        next_report_epoch=$((now_epoch + report_every_seconds))
    fi
    sleep 1
done

# Holder record. The lock is ours, so rewriting the file cannot race another
# holder; waiters only read it.
{
    printf 'pid=%s\n' "$$"
    printf 'host=%s\n' "$(hostname 2>/dev/null || uname -n)"
    printf 'started=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'cwd=%s\n' "$PWD"
    printf 'command=%s\n' "$*"
} >"$lock_path"

# Run the command with the lock descriptor closed, so nothing it spawns can
# inherit (and outlive us holding) the lock. It runs in the background so a
# signal delivered to this wrapper is forwarded instead of orphaning it. With
# setsid(1) the command leads its own process group and the whole build tree
# (make/ninja/compilers) receives the forwarded TERM; without it only the
# direct child does.
if command -v setsid >/dev/null 2>&1; then
    setsid "$@" {lock_fd}>&- &
    child_pid=$!
    signal_target="-$child_pid"
else
    "$@" {lock_fd}>&- &
    child_pid=$!
    signal_target="$child_pid"
fi
trap 'kill -TERM -- "$signal_target" 2>/dev/null' INT TERM HUP

child_status=0
while :; do
    wait "$child_pid"
    child_status=$?
    # wait returns >128 when a trapped signal interrupts it; keep waiting
    # until the child has really exited.
    kill -0 "$child_pid" 2>/dev/null || break
done

: >"$lock_path"
exit "$child_status"
