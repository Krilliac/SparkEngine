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
DXVK_VERSION="3.1.1"
DXVK_URL="https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz"
# GitHub release asset digest: doitsujin/dxvk v3.1.1 (verified 2026-10-01).
DXVK_SHA256="40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608"
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

    # The toolchain downloads and verifies the pinned DirectXMath archive.
    # Do not install mutable main-branch headers into the system include path.
    ok "DirectXMath: managed by the hash-verifying CMake toolchain"

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

install_dxvk() (
    # Always verify the archive and replace the DLLs, even on repeat setup.
    # An existing DLL or version marker is not integrity evidence.
    info "Downloading DXVK $DXVK_VERSION from GitHub..."
    tmp_dir=$(mktemp -d)
    trap 'rm -rf -- "$tmp_dir"' EXIT
    if ! curl --fail --location --proto '=https' --tlsv1.2 --retry 3 \
        "$DXVK_URL" --output "$tmp_dir/dxvk.tar.gz"; then
        fail "DXVK download failed"
        return 1
    fi
    if ! printf '%s  %s\n' "$DXVK_SHA256" "$tmp_dir/dxvk.tar.gz" | sha256sum --check --strict -; then
        fail "DXVK SHA-256 mismatch; nothing extracted or installed"
        return 1
    fi
    tar xzf "$tmp_dir/dxvk.tar.gz" -C "$tmp_dir"
    test -s "$tmp_dir/dxvk-${DXVK_VERSION}/x64/d3d11.dll"
    test -s "$tmp_dir/dxvk-${DXVK_VERSION}/x64/dxgi.dll"
    mkdir -p "$DXVK_DIR"
    cp -r "$tmp_dir/dxvk-${DXVK_VERSION}/." "$DXVK_DIR/"
    info "Verified DXVK $DXVK_VERSION installed to $DXVK_DIR"
    # Optional prefix installation is explicit: never mutate the user's default prefix.
    if [ -n "${WINEPREFIX:-}" ]; then
        local sys32="$WINEPREFIX/drive_c/windows/system32"
        if [ ! -d "$sys32" ]; then
            fail "Initialize WINEPREFIX with tools/wine-run.sh --setup-only first"
            return 1
        fi
        cp "$DXVK_DIR/x64/d3d11.dll" "$DXVK_DIR/x64/dxgi.dll" "$sys32/"
        info "DXVK copied into $WINEPREFIX; run with SPARK_WINE_BACKEND=dxvk"
    fi
)

# ============================================================================
# Install system packages (requires sudo)
# ============================================================================

install_packages() {
    info "Installing system packages (mingw-w64, wine64, mesa-vulkan-drivers)..."

    # Packages come only through APT, which checks every .deb against the signed repository index. There is
    # deliberately no direct-download fallback: re-fetching APT's URIs with wget drops that check, and an
    # on-path attacker can force such a fallback simply by corrupting APT's own download. Fail closed instead.
    if ! DEBIAN_FRONTEND=noninteractive apt-get update -qq; then
        fail "apt-get update failed; fix the package sources and re-run"
        exit 1
    fi
    if ! DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        mingw-w64 wine64 wine mesa-vulkan-drivers cmake curl ca-certificates xvfb xauth; then
        fail "apt-get install failed; nothing was installed outside APT's verification"
        exit 1
    fi

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
