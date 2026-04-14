#!/usr/bin/env bash
# wine-run.sh — Run MinGW-compiled SparkEngine .exe under Wine with DXVK
#
# This script sets up the Wine environment with DXVK (D3D11->Vulkan) and
# VKD3D-Proton (D3D12->Vulkan) so that D3D11/D3D12 code paths are exercised
# on a Linux machine. Combined with Mesa Lavapipe, this works without a GPU.
#
# Prerequisites:
#   sudo apt-get install "${WINE}" mesa-vulkan-drivers
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

# Pick the Wine launcher to use. We have to jump through hoops for Ubuntu's
# packaging:
#
#   - Debian/Ubuntu ship `/usr/bin/wine` as a shell wrapper that *always*
#     prefers `/usr/lib/wine/wine` (32-bit) when that file is test -x, even
#     for pure 64-bit .exes. On systems without working 32-bit libc
#     (minimal containers, gVisor, nested sandboxes) the 32-bit binary
#     can't even start — the wrapper fails with "Exec format error"
#     before the wrapper reaches the wine64 fallback branch.
#   - On Ubuntu 22.04+, `wine64` exists as a standalone command when
#     `apt install wine64` has been run, but the `wine64` alternative
#     isn't always linked — sometimes only `/usr/lib/wine/wine64` exists.
#
# Resolution order (stop at the first match that can actually execute a
# 64-bit .exe):
#   1. Env override: $WINE
#   2. `wine64` on $PATH (cleanest, modern distros)
#   3. `/usr/lib/wine/wine64` direct path (bypasses the wrapper)
#   4. `wine` on $PATH as a last resort (may fail with 32-bit .exe)
if [ -n "${WINE:-}" ]; then
    :  # explicit override — honor as-is
elif command -v wine64 &>/dev/null; then
    WINE="wine64"
elif [ -x "/usr/lib/wine/wine64" ]; then
    WINE="/usr/lib/wine/wine64"
    # Also export WINELOADER so Wine's own dll loader picks 64-bit.
    export WINELOADER="${WINELOADER:-/usr/lib/wine/wine64}"
elif command -v wine &>/dev/null; then
    WINE="wine"
else
    WINE="wine"
fi

check_prerequisites() {
    if ! command -v "${WINE}" &>/dev/null && [ ! -x "${WINE}" ]; then
        error "Wine not found. Install with: sudo apt-get install wine64 wine"
    fi

    if ! command -v wineboot &>/dev/null; then
        error "wineboot not found. Install with: sudo apt-get install wine64 wine"
    fi
}

# Run a tiny PE probe under Wine to verify the environment can actually
# execute 64-bit Windows binaries. Useful in minimal containers / sandboxes
# (e.g. gVisor) where Wine fails with a "Got unexpected trap 0" loop because
# the ucontext trap_no field isn't populated. Returns 0 on success,
# non-zero otherwise. Caller passes the probe path.
probe_wine_environment() {
    local probe="${1:-}"
    if [ -z "$probe" ] || [ ! -f "$probe" ]; then
        return 0  # nothing to probe
    fi

    info "Probing Wine environment with ${probe}..."
    local probe_log
    probe_log="$(mktemp)"
    local probe_rc=0
    if ! timeout 15 "${WINE}" "${probe}" > "${probe_log}" 2>&1; then
        probe_rc=$?
    fi

    # Detect the two gVisor-specific failure modes:
    #   1. "Got unexpected trap 0" loop (Wine segv_handler sees trap_no==0)
    #   2. "virtual_setup_exception stack overflow" after #1 is worked around
    if grep -q 'segv_handler Got unexpected trap 0' "${probe_log}" || \
       grep -q 'virtual_setup_exception stack overflow' "${probe_log}"; then
        echo "[wine-run] ERROR: Wine cannot execute Windows binaries in this environment." >&2
        echo "[wine-run]        Detected gVisor-style signal handling where" >&2
        echo "[wine-run]        ucontext trap_no is not populated. Wine's segv_handler" >&2
        echo "[wine-run]        loops on 'Got unexpected trap 0' during startup." >&2
        echo "[wine-run]" >&2
        echo "[wine-run]        This is a known upstream incompatibility between" >&2
        echo "[wine-run]        Wine 9.0 and gVisor's runsc user-mode kernel. Run" >&2
        echo "[wine-run]        MinGW-compiled artifacts on a Linux host with a real" >&2
        echo "[wine-run]        kernel (Docker runc, bare metal, or a conventional" >&2
        echo "[wine-run]        VM) to exercise the Wine path." >&2
        echo "[wine-run]" >&2
        echo "[wine-run]        See .claude/knowledge/wine-gvisor-incompatibility.md" >&2
        rm -f "${probe_log}"
        return 2
    fi
    rm -f "${probe_log}"
    return "${probe_rc}"
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
        "${WINE}" reg add 'HKCU\Software\Wine\DllOverrides' /v d3d11 /t REG_SZ /d native /f 2>/dev/null || true
        "${WINE}" reg add 'HKCU\Software\Wine\DllOverrides' /v dxgi /t REG_SZ /d native /f 2>/dev/null || true
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

        "${WINE}" reg add 'HKCU\Software\Wine\DllOverrides' /v d3d12 /t REG_SZ /d native /f 2>/dev/null || true
        wineserver --wait 2>/dev/null || true
        return 0
    else
        info "VKD3D-Proton not found — D3D12 tests may not run"
        info "  Install VKD3D-Proton for Vulkan-based D3D12 support"
        return 1
    fi
}

detect_gpu() {
    # Check for real GPU (not software) via Vulkan
    if command -v vulkaninfo &>/dev/null; then
        local gpu_type
        gpu_type=$(vulkaninfo 2>/dev/null | grep "deviceType" | head -1 | tr -d ' ')
        if echo "$gpu_type" | grep -qi "DISCRETE\|INTEGRATED"; then
            info "Real GPU detected — using hardware Vulkan (fast)"
            # Don't force Lavapipe if a real GPU exists
            unset VK_ICD_FILENAMES
            unset LIBGL_ALWAYS_SOFTWARE
            return 0
        fi
    fi

    # Check for GPU via /dev/dri
    if [ -d /dev/dri ] && ls /dev/dri/renderD* &>/dev/null 2>&1; then
        info "GPU render node found at /dev/dri — checking if usable"
        # Still use Lavapipe unless user overrides, as render node could be a virtual GPU
    fi

    return 1
}

setup_dxvk_cache() {
    # DXVK caches compiled Vulkan pipelines to disk. First run is slower;
    # subsequent runs reuse the cache for near-instant shader loading.
    local cache_dir="$PROJECT_ROOT/build/.dxvk-cache"
    mkdir -p "$cache_dir"
    export DXVK_STATE_CACHE_PATH="$cache_dir"
    export DXVK_STATE_CACHE=1

    if [ -f "$cache_dir/"*.dxvk-cache 2>/dev/null ]; then
        local cache_size
        cache_size=$(du -sh "$cache_dir" 2>/dev/null | cut -f1)
        info "DXVK state cache: $cache_dir ($cache_size)"
    else
        info "DXVK state cache: $cache_dir (empty — first run will populate)"
    fi
}

print_info() {
    echo "=== Wine Run Environment ==="
    echo "WINEPREFIX:   $WINEPREFIX"
    echo "VK_ICD_FILES: ${VK_ICD_FILENAMES:-<system default>}"
    echo "LIBGL_SW:     ${LIBGL_ALWAYS_SOFTWARE:-0}"
    echo "DXVK_CACHE:   ${DXVK_STATE_CACHE_PATH:-<not set>}"
    echo ""
    "${WINE}" --version 2>/dev/null || wine --version 2>/dev/null || echo "Wine: not installed"
    echo ""
    # GPU detection
    if command -v vulkaninfo &>/dev/null; then
        local gpu_name
        gpu_name=$(vulkaninfo 2>/dev/null | grep "deviceName" | head -1 | sed 's/.*= //')
        echo "Vulkan GPU:   ${gpu_name:-unknown}"
    fi
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

# Try to use real GPU if available (skips Lavapipe)
detect_gpu || true

detect_dxvk && setup_dxvk_cache || true
detect_vkd3d || true

# Enable verbose Wine logging if requested
if [ "${SPARK_WINE_LOG:-0}" = "1" ]; then
    export WINEDEBUG="warn+all"
fi

# Optional: probe the Wine environment with a pre-built hello-world .exe
# before running the real target, so sandboxed / gVisor environments fail
# fast with a clear diagnostic instead of spinning in Wine's segv loop.
# Enable by setting SPARK_WINE_PROBE=/path/to/hello.exe
if [ -n "${SPARK_WINE_PROBE:-}" ]; then
    if ! probe_wine_environment "${SPARK_WINE_PROBE}"; then
        exit 2
    fi
fi

info "Running: "${WINE}" $EXE $*"
info "  Vulkan ICD: ${VK_ICD_FILENAMES:-<system default>}"
info "  DXVK cache: ${DXVK_STATE_CACHE_PATH:-<disabled>}"

# Optional LD_PRELOAD shim to patch Wine's segv_handler under gVisor.
# Build with: gcc -shared -fPIC -o tools/gvisor-wine-shim.so tools/gvisor-wine-shim.c -ldl
# Enable by setting SPARK_WINE_GVISOR_SHIM=1 (auto-detects tools/gvisor-wine-shim.so).
if [ "${SPARK_WINE_GVISOR_SHIM:-0}" = "1" ]; then
    _shim="${PROJECT_ROOT}/tools/gvisor-wine-shim.so"
    if [ -f "${_shim}" ]; then
        export LD_PRELOAD="${_shim}${LD_PRELOAD:+:${LD_PRELOAD}}"
        info "  gVisor shim: ${_shim}"
    else
        info "  gVisor shim: not built — run 'make -C tools gvisor-wine-shim.so'"
    fi
fi

# Run under Wine
exec "${WINE}" "$EXE" "$@"
