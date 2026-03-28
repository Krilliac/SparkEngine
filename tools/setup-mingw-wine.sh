#!/usr/bin/env bash
# setup-mingw-wine.sh — Install all prerequisites for cross-compiling
# SparkEngine's Windows D3D11/D3D12 backends on Linux and running under Wine.
#
# Usage:
#   sudo tools/setup-mingw-wine.sh
#   tools/setup-mingw-wine.sh --check    # Verify installation without installing
#
# After installation, build and run:
#   cmake --preset linux-mingw-release
#   cmake --build build/linux-mingw-release
#   tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "${GREEN}[OK]${NC}    $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $1"; }
fail() { echo -e "${RED}[MISS]${NC}  $1"; }

# ============================================================================
# Check mode — verify what's installed without changing anything
# ============================================================================

check_installation() {
    echo "=== SparkEngine MinGW + Wine Cross-Compilation Check ==="
    echo ""
    local all_ok=true

    # MinGW
    if command -v x86_64-w64-mingw32-g++ &>/dev/null; then
        ok "MinGW-w64 g++: $(x86_64-w64-mingw32-g++ --version | head -1)"
    else
        fail "MinGW-w64 g++ not found"
        all_ok=false
    fi

    if command -v x86_64-w64-mingw32-gcc &>/dev/null; then
        ok "MinGW-w64 gcc: $(x86_64-w64-mingw32-gcc --version | head -1)"
    else
        fail "MinGW-w64 gcc not found"
        all_ok=false
    fi

    # Check for D3D headers in MinGW sysroot
    if [ -f /usr/x86_64-w64-mingw32/include/d3d11.h ]; then
        ok "D3D11 headers present (d3d11.h)"
    else
        fail "D3D11 headers missing"
        all_ok=false
    fi

    if [ -f /usr/x86_64-w64-mingw32/include/d3d12.h ]; then
        ok "D3D12 headers present (d3d12.h)"
    else
        fail "D3D12 headers missing"
        all_ok=false
    fi

    # Wine
    if command -v wine64 &>/dev/null; then
        ok "Wine: $(wine64 --version 2>/dev/null || echo 'version unknown')"
    else
        fail "Wine not found"
        all_ok=false
    fi

    if command -v wineboot &>/dev/null; then
        ok "wineboot available"
    else
        fail "wineboot not found"
        all_ok=false
    fi

    # Mesa Lavapipe (software Vulkan)
    local lavapipe_found=false
    for icd in \
        /usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
        /usr/share/vulkan/icd.d/lvp_icd.json; do
        if [ -f "$icd" ]; then
            ok "Lavapipe ICD: $icd"
            lavapipe_found=true
            break
        fi
    done
    if [ "$lavapipe_found" = false ]; then
        fail "Lavapipe (mesa-vulkan-drivers) not found"
        all_ok=false
    fi

    # DXVK (optional — D3D11->Vulkan translation)
    local dxvk_found=false
    for path in /usr/share/dxvk/x64 /usr/lib/dxvk /opt/dxvk/x64; do
        if [ -f "$path/d3d11.dll" ]; then
            ok "DXVK: $path"
            dxvk_found=true
            break
        fi
    done
    if [ "$dxvk_found" = false ]; then
        warn "DXVK not found (optional — Wine's built-in WineD3D will be used for D3D11)"
    fi

    # VKD3D-Proton (optional — D3D12->Vulkan translation)
    local vkd3d_found=false
    for path in /usr/share/vkd3d-proton/x64 /usr/lib/vkd3d-proton /opt/vkd3d-proton/x64; do
        if [ -f "$path/d3d12.dll" ]; then
            ok "VKD3D-Proton: $path"
            vkd3d_found=true
            break
        fi
    done
    if [ "$vkd3d_found" = false ]; then
        warn "VKD3D-Proton not found (optional — D3D12 codepaths won't be exercised at runtime)"
    fi

    # CMake
    if command -v cmake &>/dev/null; then
        ok "CMake: $(cmake --version | head -1)"
    else
        fail "CMake not found"
        all_ok=false
    fi

    echo ""
    if [ "$all_ok" = true ]; then
        echo -e "${GREEN}All required tools are installed. Ready to cross-compile.${NC}"
        echo ""
        echo "Build:  cmake --preset linux-mingw-release && cmake --build build/linux-mingw-release"
        echo "Run:    tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe"
        return 0
    else
        echo -e "${RED}Some required tools are missing. Run: sudo tools/setup-mingw-wine.sh${NC}"
        return 1
    fi
}

if [ "${1:-}" = "--check" ]; then
    check_installation
    exit $?
fi

# ============================================================================
# Install mode — must run as root
# ============================================================================

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo $0"
    exit 1
fi

echo "=== Installing MinGW + Wine Cross-Compilation Prerequisites ==="
echo ""

# Step 1: Update package lists
echo ">>> Updating package lists..."
apt-get update -qq

# Step 2: Install MinGW-w64 cross-compiler
echo ">>> Installing MinGW-w64 (cross-compiler for Windows targets)..."
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    mingw-w64 \
    cmake \
    ccache

# Step 3: Install Wine (for running .exe files)
echo ">>> Installing Wine..."
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    wine64

# Step 4: Install Mesa Lavapipe (software Vulkan for GPU-less environments)
echo ">>> Installing Mesa Lavapipe (software Vulkan)..."
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    mesa-vulkan-drivers

# Step 5: Install DXVK (optional — D3D11->Vulkan translation)
echo ">>> Installing DXVK (D3D11->Vulkan, optional)..."
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    dxvk 2>/dev/null || echo "  DXVK package not available — Wine's built-in WineD3D will be used"

echo ""
echo "=== Installation Complete ==="
echo ""

# Run verification
check_installation
