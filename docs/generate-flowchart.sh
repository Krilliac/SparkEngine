#!/bin/bash

# SparkEngine Architecture Flowchart Generator
# Scans the codebase and generates wiki/Engine-Architecture-Flowchart.md
# with complete visual flowcharts of every engine subsystem.
#
# What it does:
#   1. Scans entry points (SparkEngine, SparkEditor, SparkConsole, GameModules)
#   2. Scans all subsystem headers for classes, systems, and components
#   3. Generates ASCII flowcharts showing data flow and execution order
#   4. Writes wiki/Engine-Architecture-Flowchart.md
#
# Usage:
#   ./generate-flowchart.sh              # Generate (default)
#   ./generate-flowchart.sh generate     # Same as above
#   ./generate-flowchart.sh check        # Dry-run: report if out of date

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WIKI_DIR="$PROJECT_ROOT/wiki"
OUTPUT="$WIKI_DIR/Engine-Architecture-Flowchart.md"
SRC="$PROJECT_ROOT/SparkEngine/Source"
EDITOR_SRC="$PROJECT_ROOT/SparkEditor/Source"
CONSOLE_SRC="$PROJECT_ROOT/SparkConsole/src"
GAME_SRC="$PROJECT_ROOT/GameModules"

RED='[0;31m'
GREEN='[0;32m'
YELLOW='[1;33m'
BLUE='[0;34m'
NC='[0m'

log_info()    { echo -e "${BLUE}[FLOWCHART]${NC} $1"; }
log_success() { echo -e "${GREEN}[FLOWCHART]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[FLOWCHART]${NC} $1"; }

# ============================================================================
# Scan functions - extract live data from the codebase
# ============================================================================

count_headers() {
    find "$SRC" "$EDITOR_SRC" -name "*.h" 2>/dev/null | wc -l | tr -d " "
}

count_cpp() {
    find "$SRC" "$EDITOR_SRC" "$CONSOLE_SRC" -name "*.cpp" 2>/dev/null | wc -l | tr -d " "
}

count_ecs_components() {
    local dir="$SRC/Engine/ECS/Components"
    if [ -d "$dir" ]; then
        grep -rh "^struct " "$dir" 2>/dev/null | grep -v "//" | wc -l | tr -d " "
    else
        echo "0"
    fi
}

count_ecs_systems() {
    local file="$SRC/Engine/ECS/Systems/ECSystems.h"
    if [ -f "$file" ]; then
        grep -c "class .*System" "$file" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

count_editor_panels() {
    find "$EDITOR_SRC/Panels" -name "*Panel.h" 2>/dev/null | wc -l | tr -d " "
}

list_editor_panels() {
    find "$EDITOR_SRC/Panels" -name "*Panel.h" 2>/dev/null |         sed "s|.*/||; s|Panel\.h||; s|\([a-z]\)\([A-Z]\)| |g" |         sort
}

list_rhi_backends() {
    local rhi_dir="$SRC/Graphics/RHI"
    local backends=""
    [ -d "$rhi_dir/D3D11" ] && backends="$backends D3D11"
    [ -d "$rhi_dir/D3D12" ] && backends="$backends D3D12"
    [ -d "$rhi_dir/Vulkan" ] && backends="$backends Vulkan"
    [ -d "$rhi_dir/OpenGL" ] && backends="$backends OpenGL"
    [ -d "$rhi_dir/Metal" ] && backends="$backends Metal"
    [ -f "$rhi_dir/NullRHIDevice.h" ] && backends="$backends NullRHI"
    echo "$backends"
}

list_game_modules() {
    find "$GAME_SRC" -maxdepth 1 -mindepth 1 -type d 2>/dev/null |         sed "s|.*/||" | sort
}

# ============================================================================
# Main generation
# ============================================================================
generate() {
    log_info "Scanning codebase..."

    local n_headers n_cpp n_components n_systems n_panels
    n_headers=$(count_headers)
    n_cpp=$(count_cpp)
    n_components=$(count_ecs_components)
    n_systems=$(count_ecs_systems)
    n_panels=$(count_editor_panels)

    local backends
    backends=$(list_rhi_backends)

    local modules
    modules=$(list_game_modules)

    log_info "Found: $n_headers headers, $n_cpp sources, $n_components components, $n_systems ECS systems, $n_panels editor panels"
    log_info "RHI backends:$backends"
    log_info "Game modules: $modules"

    log_info "Generating $OUTPUT ..."

    python3 "$SCRIPT_DIR/generate-flowchart-content.py"         --output "$OUTPUT"         --headers "$n_headers"         --sources "$n_cpp"         --components "$n_components"         --systems "$n_systems"         --panels "$n_panels"         --backends "$backends"         --modules "$modules"         --project-root "$PROJECT_ROOT"

    log_success "Generated $OUTPUT"
}

# ============================================================================
# Check mode
# ============================================================================
check_mode() {
    if [ ! -f "$OUTPUT" ]; then
        log_warning "Engine-Architecture-Flowchart.md does not exist. Run: docs/generate-flowchart.sh generate"
        exit 1
    fi

    local current_hash new_hash
    current_hash=$(md5sum "$OUTPUT" | awk '{ print $1 }')

    # Generate to temp and compare
    local tmpout
    tmpout=$(mktemp)
    OUTPUT="$tmpout" generate 2>/dev/null
    new_hash=$(md5sum "$tmpout" | awk '{ print $1 }')
    rm -f "$tmpout"

    if [ "$current_hash" = "$new_hash" ]; then
        log_success "Engine-Architecture-Flowchart.md is up to date."
        exit 0
    else
        log_warning "Engine-Architecture-Flowchart.md is out of date. Run: docs/generate-flowchart.sh generate"
        exit 1
    fi
}

# ============================================================================
# Entry point
# ============================================================================
case "${1:-generate}" in
    generate) generate ;;
    check)    check_mode ;;
    *)
        echo "Usage: $0 [generate|check]"
        exit 1
        ;;
esac
