#!/usr/bin/env bash
# wine-run.sh — Run MinGW-compiled SparkEngine .exe under Wine with DXVK
#
# This script sets up the Wine environment with DXVK (D3D11->Vulkan) and
# VKD3D-Proton (D3D12->Vulkan) so that D3D11/D3D12 code paths are exercised
# on a Linux machine. Combined with Mesa Lavapipe, this works without a GPU.
#
# Prerequisites:
#   sudo apt-get install wine64 mesa-vulkan-drivers
#   # Optional: install DXVK for D3D11 support
#   # DXVK is auto-detected from /usr/share/dxvk or $DXVK_PATH
#
# Usage:
#   tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe [args...]
#   tools/wine-run.sh --setup-only          # Just configure Wine prefix, don't run
#   tools/wine-run.sh --info                # Print environment info
#
# Environment Variables:
#   WINEPREFIX      — Wine prefix directory (default: build/.wineprefix)
#   DXVK_PATH       — Path to DXVK installation (auto-detected)
#   VKD3D_PATH      — Path to VKD3D-Proton installation (auto-detected)
#   SPARK_WINE_LOG  — Set to 1 for verbose Wine debug output

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Defaults
export WINEPREFIX="${WINEPREFIX:-$PROJECT_ROOT/build/.wineprefix}"
export WINEDEBUG="${WINEDEBUG:--all}"  # Suppress Wine debug spam by default

# Use Lavapipe (software Vulkan) if no GPU is available
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
    for icd in \
        /usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
        /usr/share/vulkan/icd.d/lvp_icd.json \
        /usr/lib/x86_64-linux-gnu/libvulkan_lvp.so; do
        if [ -f "$icd" ]; then
            export VK_ICD_FILENAMES="$icd"
            break
        fi
    done
fi

# Force software rendering for Mesa
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"

# ============================================================================
# Functions
# ============================================================================

info() {
    echo "[wine-run] $*"
}

error() {
    echo "[wine-run] ERROR: $*" >&2
    exit 1
}

check_prerequisites() {
    if ! command -v wine64 &>/dev/null && ! command -v wine &>/dev/null; then
        error "Wine not found. Install with: sudo apt-get install wine64"
    fi

    if ! command -v wineboot &>/dev/null; then
        error "wineboot not found. Install with: sudo apt-get install wine64"
    fi
}

setup_wineprefix() {
    if [ ! -d "$WINEPREFIX/drive_c" ]; then
        info "Initializing Wine prefix at $WINEPREFIX"
        WINEDEBUG=-all wineboot --init 2>/dev/null || true
        # Wait for wineserver to finish
        wineserver --wait 2>/dev/null || true
        info "Wine prefix ready"
    fi
}

detect_dxvk() {
    local dxvk_path="${DXVK_PATH:-}"

    # Auto-detect DXVK installation
    if [ -z "$dxvk_path" ]; then
        for candidate in \
            "$PROJECT_ROOT/ThirdParty/dxvk/x64" \
            "$PROJECT_ROOT/build/dxvk/x64" \
            /usr/share/dxvk/x64 \
            /usr/lib/dxvk \
            /opt/dxvk/x64; do
            if [ -f "$candidate/d3d11.dll" ]; then
                dxvk_path="$candidate"
                break
            fi
        done
    fi

    if [ -n "$dxvk_path" ] && [ -f "$dxvk_path/d3d11.dll" ]; then
        info "DXVK found at $dxvk_path — D3D11 will translate to Vulkan"
        local sys32="$WINEPREFIX/drive_c/windows/system32"
        cp -f "$dxvk_path/d3d11.dll" "$sys32/" 2>/dev/null || true
        cp -f "$dxvk_path/dxgi.dll" "$sys32/" 2>/dev/null || true

        # Tell Wine to use the native DLLs
        wine64 reg add 'HKCU\Software\Wine\DllOverrides' /v d3d11 /t REG_SZ /d native /f 2>/dev/null || true
        wine64 reg add 'HKCU\Software\Wine\DllOverrides' /v dxgi /t REG_SZ /d native /f 2>/dev/null || true
        wineserver --wait 2>/dev/null || true
        return 0
    else
        info "DXVK not found — Wine's built-in D3D11 (WineD3D->OpenGL) will be used"
        info "  Install DXVK for Vulkan-based D3D11: sudo apt-get install dxvk"
        return 1
    fi
}

detect_vkd3d() {
    local vkd3d_path="${VKD3D_PATH:-}"

    if [ -z "$vkd3d_path" ]; then
        for candidate in \
            /usr/share/vkd3d-proton/x64 \
            /usr/lib/vkd3d-proton \
            /opt/vkd3d-proton/x64; do
            if [ -f "$candidate/d3d12.dll" ]; then
                vkd3d_path="$candidate"
                break
            fi
        done
    fi

    if [ -n "$vkd3d_path" ] && [ -f "$vkd3d_path/d3d12.dll" ]; then
        info "VKD3D-Proton found at $vkd3d_path — D3D12 will translate to Vulkan"
        local sys32="$WINEPREFIX/drive_c/windows/system32"
        cp -f "$vkd3d_path/d3d12.dll" "$sys32/" 2>/dev/null || true

        wine64 reg add 'HKCU\Software\Wine\DllOverrides' /v d3d12 /t REG_SZ /d native /f 2>/dev/null || true
        wineserver --wait 2>/dev/null || true
        return 0
    else
        info "VKD3D-Proton not found — D3D12 tests may not run"
        info "  Install VKD3D-Proton for Vulkan-based D3D12 support"
        return 1
    fi
}

print_info() {
    echo "=== Wine Run Environment ==="
    echo "WINEPREFIX:   $WINEPREFIX"
    echo "VK_ICD_FILES: ${VK_ICD_FILENAMES:-<default>}"
    echo "LIBGL_SW:     ${LIBGL_ALWAYS_SOFTWARE:-0}"
    echo ""
    wine64 --version 2>/dev/null || wine --version 2>/dev/null || echo "Wine: not installed"
    echo ""
    if [ -n "${VK_ICD_FILENAMES:-}" ]; then
        echo "Vulkan ICD:   $VK_ICD_FILENAMES"
    fi
    echo "============================="
}

# ============================================================================
# Main
# ============================================================================

check_prerequisites

case "${1:-}" in
    --setup-only)
        setup_wineprefix
        detect_dxvk || true
        detect_vkd3d || true
        info "Setup complete. Wine prefix at: $WINEPREFIX"
        exit 0
        ;;
    --info)
        print_info
        exit 0
        ;;
    --help|-h)
        echo "Usage: $0 <executable.exe> [args...]"
        echo "       $0 --setup-only"
        echo "       $0 --info"
        exit 0
        ;;
    "")
        error "No executable specified. Usage: $0 <executable.exe> [args...]"
        ;;
esac

EXE="$1"
shift

if [ ! -f "$EXE" ]; then
    error "File not found: $EXE"
fi

# Set up environment
setup_wineprefix
detect_dxvk || true
detect_vkd3d || true

# Enable verbose Wine logging if requested
if [ "${SPARK_WINE_LOG:-0}" = "1" ]; then
    export WINEDEBUG="warn+all"
fi

info "Running: wine64 $EXE $*"
info "  Vulkan ICD: ${VK_ICD_FILENAMES:-<system default>}"

# Run under Wine
exec wine64 "$EXE" "$@"
