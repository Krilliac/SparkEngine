#!/bin/bash
# ============================================================================
# Spark Engine - Cross-Platform Shader Compilation Pipeline
# ============================================================================
#
# Compiles GLSL shaders to SPIR-V for Vulkan backend using glslangValidator.
# Also validates GLSL shaders for OpenGL correctness.
#
# Prerequisites:
#   - glslangValidator (from Vulkan SDK or standalone)
#   - spirv-opt (optional, from SPIRV-Tools for optimization)
#
# Usage:
#   ./compile_shaders.sh              # Compile all shaders
#   ./compile_shaders.sh --validate   # Validate only, don't compile
#   ./compile_shaders.sh --optimize   # Compile with SPIR-V optimization
#   ./compile_shaders.sh --clean      # Remove compiled shaders
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GLSL_DIR="$SCRIPT_DIR/GLSL"
SPIRV_DIR="$SCRIPT_DIR/SPIRV"
HLSL_DIR="$SCRIPT_DIR/HLSL"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Options
VALIDATE_ONLY=false
OPTIMIZE=false
CLEAN=false
VERBOSE=false

for arg in "$@"; do
    case $arg in
        --validate) VALIDATE_ONLY=true ;;
        --optimize) OPTIMIZE=true ;;
        --clean)    CLEAN=true ;;
        --verbose)  VERBOSE=true ;;
        --help)
            echo "Usage: $0 [--validate] [--optimize] [--clean] [--verbose]"
            exit 0
            ;;
    esac
done

# ============================================================================
# CLEAN
# ============================================================================
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning compiled shaders...${NC}"
    rm -rf "$SPIRV_DIR"/*.spv
    echo -e "${GREEN}Done.${NC}"
    exit 0
fi

# ============================================================================
# FIND TOOLS
# ============================================================================
GLSLANG=$(command -v glslangValidator 2>/dev/null || echo "")
SPIRV_OPT=$(command -v spirv-opt 2>/dev/null || echo "")

# Check Vulkan SDK
if [ -z "$GLSLANG" ] && [ -n "$VULKAN_SDK" ]; then
    GLSLANG="$VULKAN_SDK/bin/glslangValidator"
fi

if [ -z "$GLSLANG" ] || [ ! -f "$GLSLANG" ]; then
    echo -e "${RED}Error: glslangValidator not found.${NC}"
    echo "Install the Vulkan SDK or glslang standalone."
    echo "  Ubuntu/Debian: sudo apt install glslang-tools"
    echo "  Arch:          sudo pacman -S glslang"
    echo "  macOS:         brew install glslang"
    exit 1
fi

echo -e "${BLUE}=== Spark Engine Shader Compilation ===${NC}"
echo -e "glslangValidator: $GLSLANG"
[ -n "$SPIRV_OPT" ] && echo -e "spirv-opt: $SPIRV_OPT"

mkdir -p "$SPIRV_DIR"

# ============================================================================
# COMPILE GLSL -> SPIR-V
# ============================================================================
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

compile_shader() {
    local src="$1"
    local stage="$2"
    local defines="$3"
    local basename=$(basename "$src" .glsl)
    local suffix="$4"
    local output="$SPIRV_DIR/${basename}${suffix}.spv"

    if [ "$VALIDATE_ONLY" = true ]; then
        echo -n "  Validating $basename${suffix} ($stage)... "
        if $GLSLANG -S "$stage" $defines "$src" > /dev/null 2>&1; then
            echo -e "${GREEN}OK${NC}"
            PASS_COUNT=$((PASS_COUNT + 1))
        else
            echo -e "${RED}FAIL${NC}"
            if [ "$VERBOSE" = true ]; then
                $GLSLANG -S "$stage" $defines "$src" 2>&1 || true
            fi
            FAIL_COUNT=$((FAIL_COUNT + 1))
        fi
        return
    fi

    echo -n "  Compiling $basename${suffix} ($stage)... "

    local error_output
    if error_output=$($GLSLANG -V -S "$stage" $defines -o "$output" "$src" 2>&1); then
        # Optimize if requested
        if [ "$OPTIMIZE" = true ] && [ -n "$SPIRV_OPT" ]; then
            $SPIRV_OPT -O "$output" -o "$output" 2>/dev/null || true
        fi
        local size=$(stat -c%s "$output" 2>/dev/null || stat -f%z "$output" 2>/dev/null || echo "?")
        echo -e "${GREEN}OK${NC} (${size} bytes)"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo -e "${RED}FAIL${NC}"
        if [ "$VERBOSE" = true ]; then
            echo "$error_output"
        fi
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

echo ""
echo -e "${BLUE}--- Vertex Shaders ---${NC}"
compile_shader "$GLSL_DIR/BasicVS.glsl" "vert" "" "_VS"
compile_shader "$GLSL_DIR/FullscreenQuad.glsl" "vert" "" "_VS"

echo ""
echo -e "${BLUE}--- Fragment Shaders ---${NC}"
compile_shader "$GLSL_DIR/BasicPS.glsl" "frag" "" "_PS"
compile_shader "$GLSL_DIR/PBRSurface.glsl" "frag" "" "_PS"
compile_shader "$GLSL_DIR/SSAO.glsl" "frag" "" "_PS"
compile_shader "$GLSL_DIR/BloomExtract.glsl" "frag" "" "_PS"
compile_shader "$GLSL_DIR/DebugVisualize.glsl" "frag" "" "_PS"

echo ""
echo -e "${BLUE}--- Post-Processing ---${NC}"
compile_shader "$GLSL_DIR/PostProcess.glsl" "frag" "" "_PS"
compile_shader "$GLSL_DIR/PostProcess.glsl" "frag" "-DFXAA_PASS" "_FXAA_PS"
compile_shader "$GLSL_DIR/PostProcess.glsl" "frag" "-DVIGNETTE_PASS" "_Vignette_PS"
compile_shader "$GLSL_DIR/PostProcess.glsl" "frag" "-DGRAIN_PASS" "_Grain_PS"
compile_shader "$GLSL_DIR/PostProcess.glsl" "frag" "-DCHROMATIC_PASS" "_Chromatic_PS"

echo ""
echo -e "${BLUE}--- Gaussian Blur ---${NC}"
compile_shader "$GLSL_DIR/GaussianBlur.glsl" "frag" "-DBLUR_HORIZONTAL" "_BlurH_PS"
compile_shader "$GLSL_DIR/GaussianBlur.glsl" "frag" "" "_BlurV_PS"

echo ""
echo -e "${BLUE}--- Multi-Stage Shaders (Vertex) ---${NC}"
for shader in Phong Rim Water EnvReflection; do
    if [ -f "$GLSL_DIR/$shader.glsl" ]; then
        compile_shader "$GLSL_DIR/$shader.glsl" "vert" "-DVERTEX_SHADER" "_VS"
    fi
done

echo ""
echo -e "${BLUE}--- Multi-Stage Shaders (Fragment) ---${NC}"
for shader in Phong Rim Water EnvReflection; do
    if [ -f "$GLSL_DIR/$shader.glsl" ]; then
        compile_shader "$GLSL_DIR/$shader.glsl" "frag" "-DFRAGMENT_SHADER" "_PS"
    fi
done

# ============================================================================
# SUMMARY
# ============================================================================
echo ""
echo -e "${BLUE}=== Compilation Summary ===${NC}"
echo -e "  Passed: ${GREEN}${PASS_COUNT}${NC}"
echo -e "  Failed: ${RED}${FAIL_COUNT}${NC}"
[ "$SKIP_COUNT" -gt 0 ] && echo -e "  Skipped: ${YELLOW}${SKIP_COUNT}${NC}"

if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Some shaders failed to compile. Run with --verbose for details.${NC}"
    exit 1
fi

echo -e "${GREEN}All shaders compiled successfully!${NC}"
