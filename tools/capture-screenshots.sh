#!/usr/bin/env bash
# Capture editor/console/tool screenshots on Linux via Xvfb + llvmpipe.
# Output: docs/screenshots/*.png
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/docs/screenshots"
BIN="$ROOT/build/linux-gcc-release/bin"
mkdir -p "$OUT"

export DISPLAY=:99
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_GL_VERSION_OVERRIDE=3.3
export SDL_VIDEODRIVER=x11
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/runtime-root}"
mkdir -p "$XDG_RUNTIME_DIR" 2>/dev/null
chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null || true

pgrep -f "Xvfb :99" >/dev/null || (Xvfb :99 -screen 0 1600x900x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &)
sleep 1

grab() {
    local out="$1"
    import -window root -display :99 "$out" 2>/dev/null
    if [ -f "$out" ] && [ "$(stat -c%s "$out")" -gt 1000 ]; then
        echo "  saved: $out ($(stat -c%s "$out") bytes)"
        return 0
    else
        echo "  WARN: $out tiny or missing"
        return 1
    fi
}

kill_editor() {
    pkill -9 -f SparkEditor 2>/dev/null || true
    pkill -9 -f SparkEngine 2>/dev/null || true
    pkill -9 -f SparkConsole 2>/dev/null || true
    pkill -9 -f SparkBuild 2>/dev/null || true
    pkill -9 xterm 2>/dev/null || true
    sleep 1
}

capture_editor_theme() {
    local theme="$1"
    local slug="$2"
    local waitsec="${3:-7}"
    local img="$OUT/editor-theme-${slug}.png"
    echo ">>> Editor theme: $theme"
    kill_editor
    "$BIN/SparkEditor" --test-mode --test-frames 100000 --theme "$theme" \
        --project "$ROOT" </dev/null > "$OUT/.editor-${slug}.log" 2>&1 &
    local pid=$!
    sleep "$waitsec"
    grab "$img"
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null
}

# Launch xterm running $cmd, capture full display after $wait seconds.
capture_xterm() {
    local img="$1"
    local title="$2"
    local waitsec="$3"
    shift 3
    kill_editor
    xterm -display :99 -geometry 150x40 -fa DejaVuSansMono -fs 12 \
        -bg "#0d1117" -fg "#d7d7d7" -T "$title" -hold \
        -e "$@" >/dev/null 2>&1 &
    local pid=$!
    sleep "$waitsec"
    grab "$img"
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null
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
ls -la "$OUT"/*.png 2>/dev/null | sort -k9
true
