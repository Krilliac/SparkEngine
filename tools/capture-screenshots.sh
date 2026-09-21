#!/usr/bin/env bash
# Capture editor/console/tool screenshots on Linux via Xvfb + llvmpipe.
# Output: docs/screenshots/*.png
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/docs/screenshots"
BIN="$ROOT/build/linux-gcc-release/bin"
mkdir -p "$OUT"

export LIBGL_ALWAYS_SOFTWARE=1
export MESA_GL_VERSION_OVERRIDE=3.3
export SDL_VIDEODRIVER=x11
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-root}"
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

# Every capture owns a separate process group and a persistent group leader.
# The leader stays alive until cleanup, so even descendants whose immediate
# parent exits remain attributable without looking up process names.
owned_pids=()
owned_start_times=()
owned_pgids=()
owned_native_pids=()
owned_modes=()
owned_roles=()
owned_active=()
display_lock=""
cleanup_failed=0

use_process_groups=0
if command -v setsid >/dev/null 2>&1 \
    && setsid --wait true >/dev/null 2>&1 \
    && ps -o pgid= -p "$$" >/dev/null 2>&1; then
    use_process_groups=1
fi

read_start_time() {
    local pid="$1"
    local process_stat
    local fields=()
    if { IFS= read -r process_stat <"/proc/$pid/stat"; } 2>/dev/null; then
        # The parenthesized command can contain spaces; field 22 is the
        # twentieth field after it.  Builtin reads also avoid a ps/awk process
        # for every identity check on Git Bash.
        read -r -a fields <<< "${process_stat##*) }"
        printf '%s\n' "${fields[19]-}"
    fi
}

pid_is_live() {
    local pid="$1"
    kill -0 "$pid" 2>/dev/null || return 1
    if [ -r "/proc/$pid/stat" ]; then
        [ "$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)" != Z ] || return 1
    else
        # Git Bash exposes Windows-backed children through its ps -W table,
        # but does not provide a reliable /proc zombie state.
        [ -n "$(read_windows_identity "$pid")" ] || return 1
    fi
    return 0
}

read_process_group() {
    if [ "$use_process_groups" -eq 1 ]; then
        ps -o pgid= -p "$1" 2>/dev/null | tr -d '[:space:]'
    else
        ps -l 2>/dev/null | awk -v wanted="$1" '$1 == wanted {print $3; exit}'
    fi
}

read_native_pid() {
    local native_pid
    if { IFS= read -r native_pid <"/proc/$1/winpid"; } 2>/dev/null; then
        printf '%s\n' "$native_pid"
    fi
}

read_windows_identity() {
    # PPID and process state change when a parent exits.  They must never be
    # part of the identity used to reap an already snapshotted descendant.
    ps -W -l 2>/dev/null | awk -v wanted="$1" \
        'NR > 1 && $1 == wanted {print $1, $4, $6, $7; exit}'
}

read_identity() {
    local pid="$1"
    local start_time
    start_time="$(read_start_time "$pid")"
    if [ -n "$start_time" ]; then
        printf 'start:%s\n' "$start_time"
    else
        read_windows_identity "$pid"
    fi
}

collect_group_descendants() {
    local pgid="$1"
    local leader="$2"
    # Git Bash retains PGID after PPID becomes 1.  Snapshot all members of
    # our still-owned group, with children before parents, before any signal.
    ps -l 2>/dev/null | awk -v group="$pgid" -v leader="$leader" '
        NR > 1 && $3 == group {parents[$1]=$2}
        END {
            for (pid in parents) {
                if (pid == leader) continue
                depth=0; parent=parents[pid]
                while (parent in parents && parent != pid) {
                    depth++; parent=parents[parent]
                }
                print depth, pid
            }
        }' | sort -rn | awk '{print $2}'
}

snapshot_pid_is_same() {
    local pid="$1"
    local expected="$2"
    [ -n "$expected" ] || return 1
    pid_is_live "$pid" || return 1
    [ "$(read_identity "$pid")" = "$expected" ]
}

signal_captured_descendants() {
    local snapshot="$1"
    local signal="$2"
    local force="$3"
    local child expected native
    while IFS='|' read -r child expected native; do
        [ -n "$child" ] || continue
        if snapshot_pid_is_same "$child" "$expected"; then
            if [ "$force" -eq 1 ] && [ -n "$native" ] \
                && [ "$(read_native_pid "$child")" = "$native" ]; then
                # Git Bash's builtin kill can leave a Windows-backed process
                # alive.  Its external kill supports direct Win32 termination
                # and works even with a PATH containing only /usr/bin:/bin.
                /usr/bin/kill -f -W -KILL "$native" 2>/dev/null || true
            fi
            kill -"$signal" "$child" 2>/dev/null || true
        fi
    done <<< "$snapshot"
}

captured_descendants_are_live() {
    local snapshot="$1"
    local child expected native
    while IFS='|' read -r child expected native; do
        [ -n "$child" ] || continue
        if snapshot_pid_is_same "$child" "$expected"; then
            return 0
        fi
    done <<< "$snapshot"
    return 1
}

process_group_is_alive() {
    local pgid="$1"
    ps -eo pgid=,stat= 2>/dev/null | awk -v wanted="$pgid" \
        '$1 == wanted && $2 !~ /^Z/ {found=1} END {exit !found}'
}

launch_owned() {
    local role="$1"
    local log_file="$2"
    shift 2
    local mode="tree"
    local pid
    if [ "$use_process_groups" -eq 1 ]; then
        mode="group"
        # Create the keeper before the app; never spawn a replacement during
        # cleanup.  It reserves the leader/group identity until signalled.
        # shellcheck disable=SC2016 # positional arguments expand in the child Bash
        setsid --wait bash -c '
            /usr/bin/sleep 2147483647 & keeper=$!
            "$@" &
            wait "$keeper" || :
        ' bash "$@" >"$log_file" 2>&1 &
        pid=$!
    else
        # Job control creates a dedicated group before the command can fork.
        # Keep its leader alive after the command exits so PGID cannot be
        # reused and orphaned descendants can still be snapshotted safely.
        local restore_monitor=0
        [[ "$-" == *m* ]] || restore_monitor=1
        set -m
        (set +m; /usr/bin/sleep 2147483647 & keeper=$!
            "$@" >"$log_file" 2>&1 & wait "$keeper" || :) &
        pid=$!
        [ "$restore_monitor" -eq 0 ] || set +m
    fi
    owned_pids+=("$pid")
    local identity
    identity=""
    local native_pid=""
    for _ in {1..50}; do
        identity="$(read_identity "$pid")"
        native_pid="$(read_native_pid "$pid")"
        if [ -n "$identity" ] && { [ "$mode" = group ] || [ -n "$native_pid" ]; }; then
            break
        fi
        # Completed helpers have no identity to discover.  Do not spend fifty
        # Windows process-table scans waiting for an already reaped process.
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.01
    done
    if [ -z "$identity" ]; then
        if ! pid_is_live "$pid"; then
            # A short-lived helper may have completed before Git Bash exposes
            # its process row.  It is still safe to record and reap it; there
            # is no live PID left that could be confused with a replacement.
            identity="dead:$pid"
        else
            echo "ERROR: cannot establish identity for owned child PID $pid" >&2
            exit 1
        fi
    fi
    owned_start_times+=("$identity")
    local pgid
    pgid="$(read_process_group "$pid")"
    if [ "$pgid" != "$pid" ]; then
        echo "ERROR: owned capture group was not isolated (PID $pid, PGID $pgid)" >&2
        exit 1
    fi
    owned_pgids+=("$pgid")
    owned_native_pids+=("$native_pid")
    owned_modes+=("$mode")
    owned_roles+=("$role")
    owned_active+=(1)
}

owned_pid_is_same() {
    local index="$1"
    local pid="${owned_pids[$index]}"
    local expected="${owned_start_times[$index]}"
    pid_is_live "$pid" || return 1
    if [ -n "$expected" ]; then
        [ "$(read_identity "$pid")" = "$expected" ] || return 1
    fi
    return 0
}

stop_owned_index() {
    local index="$1"
    local pid="${owned_pids[$index]}"
    local pgid="${owned_pgids[$index]-}"
    local mode="${owned_modes[$index]-tree}"
    local descendants=""
    local descendant_snapshot=""
    if [ "${owned_active[$index]-0}" -eq 0 ]; then
        return
    fi
    owned_active[index]=0
    if ! owned_pid_is_same "$index"; then
        if ! pid_is_live "$pid"; then
            wait "$pid" 2>/dev/null || true
        else
            echo "ERROR: owned capture process identity changed (PID $pid)" >&2
            cleanup_failed=1
            return 1
        fi
        return
    fi
    if [ "$mode" = tree ]; then
        descendants="$(collect_group_descendants "$pgid" "$pid")"
        local child expected native
        while read -r child; do
            [ -n "$child" ] || continue
            expected="$(read_identity "$child")"
            native="$(read_native_pid "$child")"
            [ -n "$expected" ] || continue
            descendant_snapshot+="$child|$expected|$native"
            descendant_snapshot+=$'\n'
        done <<< "$descendants"
    fi

    # Validate identity before every signal to avoid PID reuse terminating an
    # unrelated process.  The group ID is captured at launch rather than
    # assuming it equals the PID in every shell/job-control configuration.
    if [ "$mode" = group ] && [ -n "$pgid" ]; then
        kill -TERM -- "-$pgid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null || true
    else
        signal_captured_descendants "$descendant_snapshot" TERM 0
        if owned_pid_is_same "$index"; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    fi
    for _ in {1..20}; do
        if [ "$mode" = group ]; then
            if ! pid_is_live "$pid" && ! process_group_is_alive "$pgid"; then
                wait "$pid" 2>/dev/null || true
                return
            fi
        elif ! pid_is_live "$pid" && ! captured_descendants_are_live "$descendant_snapshot"; then
            wait "$pid" 2>/dev/null || true
            return
        fi
        sleep 0.1
    done
    local needs_kill=0
    if [ "$mode" = group ]; then
        if owned_pid_is_same "$index" || process_group_is_alive "$pgid"; then
            needs_kill=1
        fi
    elif owned_pid_is_same "$index"; then
        needs_kill=1
    else
        if captured_descendants_are_live "$descendant_snapshot"; then
            needs_kill=1
        fi
    fi
    if [ "$needs_kill" -eq 1 ]; then
        if [ "$mode" = group ] && [ -n "$pgid" ]; then
            kill -KILL -- "-$pgid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null || true
        else
            signal_captured_descendants "$descendant_snapshot" KILL 1
            if owned_pid_is_same "$index"; then
                local native_pid="${owned_native_pids[$index]-}"
                if [ -z "$native_pid" ]; then
                    native_pid="$(read_native_pid "$pid")"
                fi
                if [ -n "$native_pid" ] \
                    && [ "$(read_native_pid "$pid")" = "$native_pid" ]; then
                    /usr/bin/kill -f -W -KILL "$native_pid" 2>/dev/null || true
                fi
                kill -KILL "$pid" 2>/dev/null || true
            fi
        fi
    fi
    for _ in {1..20}; do
        if [ "$mode" = group ]; then
            process_group_is_alive "$pgid" || break
        else
            if ! pid_is_live "$pid" \
                && ! captured_descendants_are_live "$descendant_snapshot"; then
                break
            fi
        fi
        sleep 0.1
    done
    # Never turn a bounded cleanup deadline into an unbounded shell wait.
    if pid_is_live "$pid" || captured_descendants_are_live "$descendant_snapshot" \
        || { [ "$mode" = group ] && process_group_is_alive "$pgid"; }; then
        echo "ERROR: owned capture process survived cleanup (PID $pid)" >&2
        cleanup_failed=1
        return 1
    fi
    wait "$pid" 2>/dev/null || true
}

# shellcheck disable=SC2329 # invoked indirectly by the EXIT/INT/TERM traps
cleanup() {
    local status=$?
    trap - EXIT INT TERM HUP
    local index
    for ((index=${#owned_pids[@]}-1; index>=0; index--)); do
        stop_owned_index "$index"
    done
    if [ -n "$display_lock" ]; then
        rmdir "$display_lock" 2>/dev/null || true
    fi
    if [ "$cleanup_failed" -ne 0 ]; then
        status=1
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

# Lock a display number for this run instead of racing with another capture
# invocation or attaching to an unrelated long-lived X server.
for display_number in $(seq 90 199); do
    candidate_lock="$XDG_RUNTIME_DIR/spark-capture-display-$display_number.lock"
    if [ ! -e "/tmp/.X11-unix/X$display_number" ] && mkdir "$candidate_lock" 2>/dev/null; then
        display_lock="$candidate_lock"
        DISPLAY=":$display_number"
        export DISPLAY
        break
    fi
done
if [ -z "$display_lock" ]; then
    echo "ERROR: no free display could be locked" >&2
    exit 1
fi

launch_owned display /tmp/xvfb.log Xvfb "$DISPLAY" -screen 0 1600x900x24 -nolisten tcp
sleep 1

capture_failures=0

grab() {
    local out="$1"
    import -window root -display "$DISPLAY" "$out" 2>/dev/null
    if [ -f "$out" ] && [ "$(stat -c%s "$out")" -gt 1000 ]; then
        echo "  saved: $out ($(stat -c%s "$out") bytes)"
        return 0
    else
        echo "  WARN: $out tiny or missing"
        capture_failures=$((capture_failures + 1))
        return 1
    fi
}

kill_editor() {
    local index
    for ((index=${#owned_pids[@]}-1; index>=0; index--)); do
        if [ "${owned_roles[$index]-}" = app ]; then
            stop_owned_index "$index"
        fi
    done
}

capture_editor_theme() {
    local theme="$1"
    local slug="$2"
    local waitsec="${3:-7}"
    local img="$OUT/editor-theme-${slug}.png"
    echo ">>> Editor theme: $theme"
    kill_editor
    launch_owned app "$OUT/.editor-${slug}.log" "$BIN/SparkEditor" \
        --test-mode --test-frames 100000 --theme "$theme" --project "$ROOT" </dev/null
    sleep "$waitsec"
    grab "$img"
    stop_owned_index "$((${#owned_pids[@]} - 1))"
}

# Launch xterm running $cmd, capture full display after $wait seconds.
capture_xterm() {
    local img="$1"
    local title="$2"
    local waitsec="$3"
    shift 3
    kill_editor
    launch_owned app /dev/null xterm -display "$DISPLAY" -geometry 150x40 -fa DejaVuSansMono -fs 12 \
        -bg "#0d1117" -fg "#d7d7d7" -T "$title" -hold \
        -e "$@"
    sleep "$waitsec"
    grab "$img"
    stop_owned_index "$((${#owned_pids[@]} - 1))"
}

capture_console() {
    echo ">>> SparkConsole (standalone terminal)"
    capture_xterm "$OUT/tool-sparkconsole-terminal.png" "SparkConsole (standalone)" 3 \
        "$BIN/SparkConsole"
}

capture_engine_headless() {
    echo ">>> SparkEngine (headless, NullRHIDevice startup log)"
    capture_xterm "$OUT/tool-sparkengine-headless-terminal.png" "SparkEngine (headless)" 5 \
        "bash -c 'cd $ROOT && timeout 3 $BIN/SparkEngine </dev/null 2>&1 | head -34; echo; echo \"[engine exited — headless startup shown above]\"'"
}

capture_shader_compiler_help() {
    echo ">>> SparkShaderCompiler --help"
    capture_xterm "$OUT/tool-sparkshadercompiler-help.png" "SparkShaderCompiler" 2 \
        "bash -c '$BIN/SparkShaderCompiler --help 2>&1'"
}

capture_spark_build_help() {
    echo ">>> SparkBuild --help"
    capture_xterm "$OUT/tool-sparkbuild-help.png" "SparkBuild" 2 \
        "bash -c '$BIN/SparkBuild --help 2>&1; echo; $BIN/SparkBuild --version 2>&1'"
}

capture_collab_server() {
    echo ">>> SparkEditor --collab-server (headless collaborative edit server)"
    capture_xterm "$OUT/tool-sparkeditor-collab-server.png" "SparkEditor (collab-server)" 5 \
        "bash -c '$BIN/SparkEditor --collab-server --collab-port 27099 --collab-name SparkEditDemo 2>&1 | head -20; echo; echo \"(server still running; Ctrl+C to stop)\"'"
}

# All themes registered in EditorTheme::InitializeDefaultThemes()
declare -a THEMES=(
    "Spark Professional|spark-professional"
    "Spark Fusion|spark-fusion"
    "Spark Ember|spark-ember"
    "Unity Pro|unity-pro"
    "Unreal Pro|unreal-pro"
    "VS Pro|vs-pro"
    "JetBrains|jetbrains"
    "Professional Light|professional-light"
    "High Contrast|high-contrast"
    "Blue Accent|blue-accent"
    "Orange Accent|orange-accent"
)

run_themes() {
    for t in "${THEMES[@]}"; do
        IFS='|' read -r name slug <<<"$t"
        capture_editor_theme "$name" "$slug"
    done
}

run_tools() {
    capture_console
    capture_engine_headless
    capture_shader_compiler_help
    capture_spark_build_help
    capture_collab_server
}

case "${1:-all}" in
    themes)   run_themes ;;
    tools)    run_tools ;;
    console)  capture_console ;;
    engine)   capture_engine_headless ;;
    shader)   capture_shader_compiler_help ;;
    build)    capture_spark_build_help ;;
    collab)   capture_collab_server ;;
    all)      run_themes; run_tools ;;
    *) echo "usage: $0 [all|themes|tools|console|engine|shader|build|collab]"; exit 1 ;;
esac

kill_editor
echo
echo "=== Output summary ==="
find "$OUT" -maxdepth 1 -type f -name '*.png' -print 2>/dev/null | sort
if [ "$capture_failures" -ne 0 ]; then
    echo "ERROR: $capture_failures screenshot capture(s) failed"
    exit 1
fi
exit 0
