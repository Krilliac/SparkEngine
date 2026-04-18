#!/bin/bash

# SparkEngine Codebase Statistics Auto-Generator
# Scans the live codebase and regenerates wiki/Codebase-Statistics.md
# with accurate, current metrics. No external dependencies.
#
# Usage:
#   ./update-codebase-stats.sh              # Generate (default)
#   ./update-codebase-stats.sh generate     # Same as above
#   ./update-codebase-stats.sh check        # Dry-run: report if out of date (exit 1 if stale)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
WIKI_DIR="$PROJECT_ROOT/wiki"
OUTPUT="$WIKI_DIR/advanced/Codebase-Statistics.md"

SRC="$PROJECT_ROOT/SparkEngine/Source"
EDITOR_SRC="$PROJECT_ROOT/SparkEditor/Source"
CONSOLE_SRC="$PROJECT_ROOT/SparkConsole/src"
SHADER_SRC="$PROJECT_ROOT/SparkShaderCompiler/src"
GAME_SRC="$PROJECT_ROOT/GameModules"
SDK_SRC="$PROJECT_ROOT/SparkSDK"
TEST_DIR="$PROJECT_ROOT/Tests"
SHADER_DIR="$PROJECT_ROOT/Shaders"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[STATS]${NC} $1"; }
log_success() { echo -e "${GREEN}[STATS]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[STATS]${NC} $1"; }

# ============================================================================
# Counting functions — single source of truth
# ============================================================================

count_lines() {
    local dir="$1"
    if [ -d "$dir" ]; then
        find "$dir" -name '*.h' -o -name '*.hpp' -o -name '*.cpp' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}'
    else
        echo "0"
    fi
}

count_files() {
    local dir="$1"
    local ext="$2"
    if [ -d "$dir" ]; then
        find "$dir" -name "*.$ext" 2>/dev/null | wc -l | tr -d " "
    else
        echo "0"
    fi
}

count_h() { count_files "$1" "h"; }
count_cpp() { count_files "$1" "cpp"; }

# Count lines for a specific subdirectory of SparkEngine/Source
engine_subsystem_lines() {
    local subdir="$SRC/$1"
    if [ -d "$subdir" ]; then
        find "$subdir" -name '*.h' -o -name '*.cpp' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}'
    else
        echo "0"
    fi
}

# Top N largest files in a directory
largest_files() {
    local dir="$1"
    local ext="$2"
    local n="${3:-10}"
    find "$dir" -name "*.$ext" 2>/dev/null | xargs wc -l 2>/dev/null | sort -rn | grep -v " total$" | head -n "$n" | while read -r lines file; do
        printf "| \`%s\` | %s |\n" "$(basename "$file")" "$lines"
    done
}

format_number() {
    printf "%'d" "$1" 2>/dev/null || echo "$1"
}

# ============================================================================
# Collect all metrics
# ============================================================================
collect_metrics() {
    log_info "Scanning codebase..."

    # Line counts per module
    LINES_ENGINE=$(count_lines "$SRC")
    LINES_EDITOR=$(count_lines "$EDITOR_SRC")
    LINES_GAME=$(count_lines "$GAME_SRC")
    LINES_TESTS=$(count_lines "$TEST_DIR")
    LINES_CONSOLE=$(count_lines "$CONSOLE_SRC")
    LINES_SHADER_COMPILER=$(count_lines "$SHADER_SRC")
    LINES_TOTAL=$((LINES_ENGINE + LINES_EDITOR + LINES_GAME + LINES_TESTS + LINES_CONSOLE + LINES_SHADER_COMPILER))

    # File counts
    TOTAL_H=$(find "$SRC" "$EDITOR_SRC" "$CONSOLE_SRC" "$GAME_SRC" "$SHADER_SRC" "$SDK_SRC" "$TEST_DIR" -name '*.h' -o -name '*.hpp' 2>/dev/null | wc -l | tr -d " ")
    TOTAL_CPP=$(find "$SRC" "$EDITOR_SRC" "$CONSOLE_SRC" "$GAME_SRC" "$SHADER_SRC" "$TEST_DIR" -name '*.cpp' 2>/dev/null | wc -l | tr -d " ")
    HLSL_COUNT=$(find "$SHADER_DIR" -name '*.hlsl' 2>/dev/null | wc -l | tr -d " ")
    GLSL_COUNT=$(find "$SHADER_DIR" -name '*.glsl' 2>/dev/null | wc -l | tr -d " ")
    TEST_FILE_COUNT=$(find "$TEST_DIR" -name 'Test*.cpp' 2>/dev/null | wc -l | tr -d " ")
    TEST_CASE_COUNT=$(grep -rh '^TEST(' "$TEST_DIR"/*.cpp 2>/dev/null | wc -l | tr -d " ")
    WIKI_COUNT=$(find "$WIKI_DIR" -name '*.md' ! -name '_Sidebar.md' 2>/dev/null | wc -l | tr -d " ")
    AS_COUNT=$(find "$PROJECT_ROOT/Assets" -name '*.as' 2>/dev/null | wc -l | tr -d " ")

    # Code density (engine source only, not tests/gamemodules)
    local engine_cpp_count engine_h_count
    engine_cpp_count=$(find "$SRC" "$EDITOR_SRC" -name '*.cpp' 2>/dev/null | wc -l | tr -d " ")
    engine_h_count=$(find "$SRC" "$EDITOR_SRC" -name '*.h' 2>/dev/null | wc -l | tr -d " ")
    if [ "$engine_cpp_count" -gt 0 ]; then
        AVG_CPP=$(( (LINES_ENGINE + LINES_EDITOR) / engine_cpp_count ))
    else
        AVG_CPP=0
    fi
    if [ "$engine_h_count" -gt 0 ]; then
        AVG_H=$(( (LINES_ENGINE + LINES_EDITOR) / engine_h_count ))
    else
        AVG_H=0
    fi

    # Engine subsystem lines
    LINES_GRAPHICS=$(engine_subsystem_lines "Graphics")
    LINES_ENGINE_SUB=$(engine_subsystem_lines "Engine")
    LINES_UTILS=$(engine_subsystem_lines "Utils")
    LINES_CORE=$(engine_subsystem_lines "Core")
    LINES_PHYSICS=$(engine_subsystem_lines "Physics")
    LINES_AUDIO=$(engine_subsystem_lines "Audio")
    LINES_INPUT=$(engine_subsystem_lines "Input")
    LINES_SCENE=$(engine_subsystem_lines "SceneManager")
    LINES_ENUMS=$(engine_subsystem_lines "Enums")
    LINES_GAME_DIR=$(engine_subsystem_lines "Game")
    LINES_CAMERA=$(engine_subsystem_lines "Camera")

    # Engine/... subsystems
    local subsystems=("AI" "Networking" "ECS" "Animation" "Gameplay" "Scripting" "SaveSystem" "UI" "World" "Dialogue" "Cinematic" "Modding" "2D" "Coroutine" "Replay" "Streaming" "Tween" "Destruction" "Localization" "Mobile" "Events" "Loading" "VR" "Persistence" "Physics" "Editor")
    declare -g -A SUBSYSTEM_LINES
    for sub in "${subsystems[@]}"; do
        SUBSYSTEM_LINES[$sub]=$(engine_subsystem_lines "Engine/$sub")
    done

    # ECS metrics
    COMP_HEADERS=$(find "$SRC/Engine/ECS/Components" -name '*.h' 2>/dev/null | wc -l | tr -d " ")
    COMP_STRUCTS=$(grep -rh "^struct " "$SRC/Engine/ECS/Components" 2>/dev/null | grep -v "//" | wc -l | tr -d " ")
    ECS_SYSTEMS=$(grep -c "class .*System" "$SRC/Engine/ECS/Systems/ECSystems.h" 2>/dev/null || echo "0")

    # Editor metrics
    PANEL_COUNT=$(find "$EDITOR_SRC/Panels" -name '*Panel.h' 2>/dev/null | wc -l | tr -d " ")

    # Build system
    CMAKE_OPTIONS=$(grep -rh "^option\|^cmake_dependent_option" "$PROJECT_ROOT/CMakeLists.txt" "$PROJECT_ROOT/cmake/"*.cmake 2>/dev/null | wc -l | tr -d " ")
    ENABLE_TOGGLES=$(grep -rh "^option.*ENABLE_" "$PROJECT_ROOT/CMakeLists.txt" "$PROJECT_ROOT/cmake/"*.cmake 2>/dev/null | wc -l | tr -d " ")

    # Game modules
    GAME_MODULE_COUNT=$(find "$GAME_SRC" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | wc -l | tr -d " ")

    # SDK headers
    SDK_HEADER_COUNT=$(find "$SDK_SRC" -name '*.h' 2>/dev/null | wc -l | tr -d " ")

    log_info "Collected: ${LINES_TOTAL} LOC, ${TOTAL_H} headers, ${TOTAL_CPP} .cpp, ${TEST_CASE_COUNT} tests"
}

# ============================================================================
# Generate the markdown page
# ============================================================================
generate_page() {
    local today
    today=$(date +%Y-%m-%d)

    cat << HEREDOC
# Codebase Statistics

Comprehensive metrics and analysis of the SparkEngine codebase. Updated ${today}.

## Code Volume

### Total Lines of Code

| Section | Lines |
|---------|------:|
| **SparkEngine/Source** | $(format_number "$LINES_ENGINE") |
| **SparkEditor/Source** | $(format_number "$LINES_EDITOR") |
| **GameModules** | $(format_number "$LINES_GAME") |
| **Tests** | $(format_number "$LINES_TESTS") |
| **SparkConsole/src** | $(format_number "$LINES_CONSOLE") |
| **SparkShaderCompiler/src** | $(format_number "$LINES_SHADER_COMPILER") |
| **Total C++ (excl. ThirdParty)** | **~$(format_number "$LINES_TOTAL")** |

### File Counts

| Category | Count |
|----------|------:|
| Header files (.h/.hpp) | ${TOTAL_H} |
| Implementation files (.cpp) | ${TOTAL_CPP} |
| HLSL shader files | ${HLSL_COUNT} |
| GLSL shader files | ${GLSL_COUNT} |
| AngelScript files (.as) | ${AS_COUNT} |
| Test files (.cpp) | ${TEST_FILE_COUNT} |
| Wiki pages (.md) | ${WIKI_COUNT} |

### Code Density

| Metric | Value |
|--------|-------|
| Average lines per .cpp file | ~${AVG_CPP} |
| Average lines per .h file | ~${AVG_H} |
| Largest codebase section | Graphics ($(format_number "$LINES_GRAPHICS") lines — $((LINES_GRAPHICS * 100 / LINES_ENGINE))% of SparkEngine/Source) |

## SparkEngine/Source Breakdown

### By Subsystem (Top-Level)

| Subsystem | Lines | % of Source |
|-----------|------:|:----------:|
| Graphics | $(format_number "$LINES_GRAPHICS") | $((LINES_GRAPHICS * 100 / LINES_ENGINE)).$(( (LINES_GRAPHICS * 1000 / LINES_ENGINE) % 10))% |
| Engine (all subsystems) | $(format_number "$LINES_ENGINE_SUB") | $((LINES_ENGINE_SUB * 100 / LINES_ENGINE)).$(( (LINES_ENGINE_SUB * 1000 / LINES_ENGINE) % 10))% |
| Utils | $(format_number "$LINES_UTILS") | $((LINES_UTILS * 100 / LINES_ENGINE)).$(( (LINES_UTILS * 1000 / LINES_ENGINE) % 10))% |
| Core | $(format_number "$LINES_CORE") | $((LINES_CORE * 100 / LINES_ENGINE)).$(( (LINES_CORE * 1000 / LINES_ENGINE) % 10))% |
| Physics | $(format_number "$LINES_PHYSICS") | $((LINES_PHYSICS * 100 / LINES_ENGINE)).$(( (LINES_PHYSICS * 1000 / LINES_ENGINE) % 10))% |
| Audio | $(format_number "$LINES_AUDIO") | $((LINES_AUDIO * 100 / LINES_ENGINE)).$(( (LINES_AUDIO * 1000 / LINES_ENGINE) % 10))% |
| Input | $(format_number "$LINES_INPUT") | $((LINES_INPUT * 100 / LINES_ENGINE)).$(( (LINES_INPUT * 1000 / LINES_ENGINE) % 10))% |
| SceneManager | $(format_number "$LINES_SCENE") | $((LINES_SCENE * 100 / LINES_ENGINE)).$(( (LINES_SCENE * 1000 / LINES_ENGINE) % 10))% |
| Enums | $(format_number "$LINES_ENUMS") | $((LINES_ENUMS * 100 / LINES_ENGINE)).$(( (LINES_ENUMS * 1000 / LINES_ENGINE) % 10))% |
| Game | $(format_number "$LINES_GAME_DIR") | $((LINES_GAME_DIR * 100 / LINES_ENGINE)).$(( (LINES_GAME_DIR * 1000 / LINES_ENGINE) % 10))% |
| Camera | $(format_number "$LINES_CAMERA") | $((LINES_CAMERA * 100 / LINES_ENGINE)).$(( (LINES_CAMERA * 1000 / LINES_ENGINE) % 10))% |

### Engine Subsystems (SparkEngine/Source/Engine/)

HEREDOC

    # Generate engine subsystem table dynamically
    echo "| Subsystem | Lines |"
    echo "|-----------|------:|"
    for sub in AI Networking ECS Animation Gameplay Scripting SaveSystem UI World Dialogue Cinematic Modding 2D Coroutine Replay Streaming Tween Destruction Localization Mobile Events Loading VR Persistence Physics Editor; do
        local lines="${SUBSYSTEM_LINES[$sub]}"
        if [ "$lines" -gt 0 ] 2>/dev/null; then
            echo "| $sub | $(format_number "$lines") |"
        fi
    done | sort -t'|' -k3 -rn

    cat << HEREDOC

## ECS Architecture Metrics

| Metric | Count |
|--------|------:|
| Component header files | ${COMP_HEADERS} |
| Component struct definitions | ${COMP_STRUCTS} |
| ECS systems | ${ECS_SYSTEMS} |
| Execution order | Physics → Animation → AI → Audio → Lifecycle → Render |

## Editor Metrics

| Metric | Count |
|--------|------:|
| Editor panel classes | ${PANEL_COUNT} |
| Total editor lines | $(format_number "$LINES_EDITOR") |

## Testing Metrics

| Metric | Count |
|--------|------:|
| Test files | ${TEST_FILE_COUNT} |
| TEST() definitions | $(format_number "$TEST_CASE_COUNT") |
| Subsystems covered | All major |
| Sanitizer coverage | ASan + UBSan + LSan + TSan + MSan |

## Build System Metrics

| Metric | Count |
|--------|------:|
| CMake option() declarations | ${CMAKE_OPTIONS} |
| ENABLE_* feature toggles | ${ENABLE_TOGGLES} |
| Game modules | ${GAME_MODULE_COUNT} |
| SDK public headers | ${SDK_HEADER_COUNT} |
| Supported compilers | MSVC v143/v144, GCC 13+, Clang 17+, Apple Clang, MinGW-w64 |
| Platforms | Windows, Linux, macOS (experimental) |

## Third-Party Dependencies

### Git Submodules

| Library | Path | Purpose |
|---------|------|---------|
| Dear ImGui | \`ThirdParty/UI/imgui\` | Immediate-mode GUI |
| EnTT | \`ThirdParty/ECS/entt\` | Entity Component System |
| Jolt Physics | \`ThirdParty/Physics/JoltPhysics\` | Physics engine |
| AngelScript | \`ThirdParty/Scripting/angelscript-mirror\` | Scripting VM |
| miniz | \`ThirdParty/Utils/miniz\` | Compression |
| Recast Navigation | \`ThirdParty/AI/recastnavigation\` | NavMesh pathfinding |

### Embedded Libraries

| Library | Purpose |
|---------|---------|
| DirectXTK | DirectX 11 toolkit |
| Assimp | 3D model import (FBX, glTF) |
| ImGuizmo | 3D editor gizmos |
| imnodes | Node graph editor |
| GLM | Math library |
| RapidJSON | JSON parsing |
| spdlog | Structured logging |
| stb | Image loading |
| miniaudio | Cross-platform audio fallback |

## Largest Files

### SparkEngine .cpp Files (by line count)

| File | Lines |
|------|------:|
HEREDOC
    largest_files "$SRC" "cpp" 10

    cat << HEREDOC

### SparkEngine .h Files (by line count)

| File | Lines |
|------|------:|
HEREDOC
    largest_files "$SRC" "h" 10

    cat << HEREDOC

### SparkEditor .cpp Files (by line count)

| File | Lines |
|------|------:|
HEREDOC
    largest_files "$EDITOR_SRC" "cpp" 10

    cat << HEREDOC

## Shader Inventory

| Type | Count | Location |
|------|------:|----------|
| HLSL shaders | ${HLSL_COUNT} | \`Shaders/HLSL/\` (includes Compute, MeshShaders, RayTracing) |
| GLSL shaders | ${GLSL_COUNT} | \`Shaders/GLSL/\` |
| Compiled bytecode (.cso) | varies | \`Shaders/Compiled/\` |

---

## See Also

- [Architecture Overview](../getting-started/Architecture-Overview.md) — Engine design and structure
- [Codebase Health](Codebase-Health.md) — System maturity status and known gaps
- [Testing](Testing.md) — Test suite details and CI integration
- [Build System and CMake Modules](Build-System-and-CMake-Modules.md) — Build configuration
HEREDOC
}

# ============================================================================
# Main
# ============================================================================
generate() {
    collect_metrics
    log_info "Generating $OUTPUT ..."
    generate_page > "$OUTPUT"
    log_success "Generated $OUTPUT"
}

check_mode() {
    if [ ! -f "$OUTPUT" ]; then
        log_warning "Codebase-Statistics.md does not exist. Run: docs/update-codebase-stats.sh generate"
        exit 1
    fi

    local current_hash
    current_hash=$(md5sum "$OUTPUT" | awk '{ print $1 }')

    collect_metrics
    local tmpout
    tmpout=$(mktemp)
    generate_page > "$tmpout"
    local new_hash
    new_hash=$(md5sum "$tmpout" | awk '{ print $1 }')
    rm -f "$tmpout"

    if [ "$current_hash" = "$new_hash" ]; then
        log_success "Codebase-Statistics.md is up to date."
        exit 0
    else
        log_warning "Codebase-Statistics.md is out of date. Run: docs/update-codebase-stats.sh generate"
        exit 1
    fi
}

case "${1:-generate}" in
    generate|full) generate ;;
    check)         check_mode ;;
    *)
        echo "Usage: $0 [generate|check]"
        exit 1
        ;;
esac
