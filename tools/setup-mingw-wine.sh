#!/usr/bin/env bash
# setup-mingw-wine.sh — Install all prerequisites for cross-compiling
# SparkEngine's Windows D3D11 backend on Linux and running under Wine.
#
# Usage:
#   sudo tools/setup-mingw-wine.sh          # Install everything
#   tools/setup-mingw-wine.sh --check       # Verify installation (no sudo)
#   tools/setup-mingw-wine.sh --dxvk-only   # Install DXVK only (no sudo)
#
# After installation, build and run:
#   cmake --preset linux-mingw-release
#   cmake --build build/linux-mingw-release --parallel $(nproc)
#   tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXVK_VERSION="2.5.3"
DXVK_DIR="$PROJECT_ROOT/ThirdParty/dxvk"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "${GREEN}[OK]${NC}    $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC}  $1"; }
fail() { echo -e "${RED}[MISS]${NC}  $1"; }
info() { echo -e "${GREEN}[setup]${NC} $1"; }

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

    # D3D headers
    if [ -f /usr/x86_64-w64-mingw32/include/d3d11.h ]; then
        ok "D3D11 headers present (d3d11.h)"
    else
        fail "D3D11 headers missing"
        all_ok=false
    fi

    # DirectXMath
    if [ -f /usr/x86_64-w64-mingw32/include/DirectXMath.h ]; then
        ok "DirectXMath headers present"
    else
        fail "DirectXMath.h not found — run: sudo tools/setup-mingw-wine.sh"
        all_ok=false
    fi

    # Wine
    if command -v wine64 &>/dev/null || [ -f /usr/lib/wine/wine64 ]; then
        local wine_ver
        wine_ver=$(wine64 --version 2>/dev/null || /usr/lib/wine/wine64 --version 2>/dev/null || echo "unknown")
        ok "Wine: $wine_ver"
    else
        fail "Wine not found"
        all_ok=false
    fi

    if command -v wineboot &>/dev/null; then
        ok "wineboot available"
    else
        warn "wineboot not found (wrapper may be needed)"
    fi

    # Mesa Lavapipe
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

    # DXVK
    local dxvk_found=false
    for path in "$DXVK_DIR/x64" /usr/share/dxvk/x64 /usr/lib/dxvk /opt/dxvk/x64; do
        if [ -f "$path/d3d11.dll" ]; then
            ok "DXVK: $path (D3D11->Vulkan, ~20x faster than WineD3D)"
            dxvk_found=true
            break
        fi
    done
    if [ "$dxvk_found" = false ]; then
        warn "DXVK not found — WineD3D will be used (VERY slow, minutes/frame)"
        warn "  Install with: tools/setup-mingw-wine.sh --dxvk-only"
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
        echo "Build:  cmake --preset linux-mingw-release && cmake --build build/linux-mingw-release --parallel \$(nproc)"
        echo "Test:   tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe"
        echo "Full:   python3 tools/test-windows-wine.py --build-dir build/linux-mingw-release"
        return 0
    else
        echo -e "${RED}Some required tools are missing. Run: sudo tools/setup-mingw-wine.sh${NC}"
        return 1
    fi
}

# ============================================================================
# Install DXVK from GitHub (no sudo required)
# ============================================================================

install_dxvk() {
    if [ -f "$DXVK_DIR/x64/d3d11.dll" ]; then
        info "DXVK $DXVK_VERSION already installed at $DXVK_DIR"
        return 0
    fi

    info "Downloading DXVK $DXVK_VERSION from GitHub..."
    local tmp_dir
    tmp_dir=$(mktemp -d)
    local url="https://github.com/doitsujin/dxvk/releases/download/v${DXVK_VERSION}/dxvk-${DXVK_VERSION}.tar.gz"

    if wget -q "$url" -O "$tmp_dir/dxvk.tar.gz" 2>/dev/null; then
        info "Extracting..."
        tar xzf "$tmp_dir/dxvk.tar.gz" -C "$tmp_dir"
        mkdir -p "$DXVK_DIR"
        cp -r "$tmp_dir/dxvk-${DXVK_VERSION}/"* "$DXVK_DIR/"
        rm -rf "$tmp_dir"
        info "DXVK $DXVK_VERSION installed to $DXVK_DIR"
        info "  D3D11 rendering will be ~20x faster than WineD3D"
    else
        warn "DXVK download failed — WineD3D fallback will be used"
        warn "  Download manually: $url"
        rm -rf "$tmp_dir"
        return 1
    fi
}

# ============================================================================
# Install DirectXMath headers (requires sudo)
# ============================================================================

install_directxmath() {
    if [ -f /usr/x86_64-w64-mingw32/include/DirectXMath.h ]; then
        info "DirectXMath headers already installed"
        return 0
    fi

    info "Downloading DirectXMath headers from Microsoft GitHub..."
    local tmp_dir
    tmp_dir=$(mktemp -d)
    local base_url="https://raw.githubusercontent.com/microsoft/DirectXMath/main/Inc"
    local all_ok=true

    for f in DirectXMath.h DirectXMathConvert.inl DirectXMathMatrix.inl \
             DirectXMathMisc.inl DirectXMathVector.inl DirectXCollision.h \
             DirectXCollision.inl DirectXColors.h DirectXPackedVector.h \
             DirectXPackedVector.inl; do
        if ! wget -q "$base_url/$f" -O "$tmp_dir/$f" 2>/dev/null; then
            fail "Failed to download $f"
            all_ok=false
        fi
    done

    if [ "$all_ok" = true ]; then
        cp "$tmp_dir"/DirectX*.h "$tmp_dir"/DirectX*.inl /usr/x86_64-w64-mingw32/include/
        info "DirectXMath headers installed to /usr/x86_64-w64-mingw32/include/"
    else
        fail "Some DirectXMath headers failed to download"
    fi

    rm -rf "$tmp_dir"
}

# ============================================================================
# Install system packages (requires sudo)
# ============================================================================

install_packages() {
    info "Installing system packages (mingw-w64, wine64, mesa-vulkan-drivers)..."

    DEBIAN_FRONTEND=noninteractive apt-get update -qq 2>/dev/null || true
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        mingw-w64 wine64 mesa-vulkan-drivers cmake 2>/dev/null || {
        warn "apt-get install failed — trying direct package download..."
        install_packages_direct
        return
    }

    # Create wine64 symlink if needed
    if ! command -v wine64 &>/dev/null && [ -f /usr/lib/wine/wine64 ]; then
        ln -sf /usr/lib/wine/wine64 /usr/bin/wine64
        info "Created /usr/bin/wine64 symlink"
    fi

    # Create wineboot wrapper if needed
    if ! command -v wineboot &>/dev/null; then
        printf '#!/bin/sh\nexec /usr/bin/wine64 wineboot "$@"\n' > /usr/bin/wineboot
        chmod +x /usr/bin/wineboot
        info "Created wineboot wrapper"
    fi
}

install_packages_direct() {
    # Fallback: download .deb packages directly via wget
    local tmp_dir
    tmp_dir=$(mktemp -d)

    local urls
    urls=$(apt-get download --print-uris mingw-w64-x86-64-dev g++-mingw-w64-x86-64-posix \
           gcc-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix-runtime \
           gcc-mingw-w64-base binutils-mingw-w64-x86-64 mingw-w64-common \
           libz-mingw-w64 wine64 libwine fonts-wine mesa-vulkan-drivers 2>&1 | \
           grep "^'" | sed "s/' .*//" | sed "s/^'//") || true

    local count=0
    for url in $urls; do
        wget -q "$url" -P "$tmp_dir/" 2>/dev/null && ((count++)) || warn "Failed: $url"
    done

    if [ "$count" -gt 0 ]; then
        dpkg -i --force-depends "$tmp_dir"/*.deb 2>/dev/null || true
        info "Installed $count packages via direct download"
    else
        fail "No packages could be downloaded"
    fi

    rm -rf "$tmp_dir"
}

# ============================================================================
# Main
# ============================================================================

case "${1:-}" in
    --check)
        check_installation
        exit $?
        ;;
    --dxvk-only)
        install_dxvk
        exit $?
        ;;
    --help|-h)
        echo "Usage: $0 [--check|--dxvk-only|--help]"
        echo ""
        echo "  (no args)    Install everything (requires sudo)"
        echo "  --check      Verify installation (no sudo)"
        echo "  --dxvk-only  Install DXVK to ThirdParty/dxvk (no sudo)"
        echo "  --help       Show this help"
        exit 0
        ;;
esac

# Full installation — requires root
if [ "$(id -u)" -ne 0 ]; then
    echo "Full installation requires root. Run: sudo $0"
    echo ""
    echo "Or:"
    echo "  $0 --check       # Check current status"
    echo "  $0 --dxvk-only   # Install DXVK only (no root)"
    exit 1
fi

echo "=== Installing MinGW + Wine Cross-Compilation Prerequisites ==="
echo ""

install_packages
install_directxmath

# DXVK installation (try as non-root user if possible)
if [ -n "${SUDO_USER:-}" ]; then
    su -c "cd $PROJECT_ROOT && $0 --dxvk-only" "$SUDO_USER" 2>/dev/null || install_dxvk
else
    install_dxvk
fi

echo ""
info "=== Setup complete ==="
echo ""

# Run verification
check_installation
