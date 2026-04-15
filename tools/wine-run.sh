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
#   WINEPREFIX            — Wine prefix directory (default: build/.wineprefix)
#   DXVK_PATH             — Path to DXVK installation (auto-detected)
#   VKD3D_PATH            — Path to VKD3D-Proton installation (auto-detected)
#   SPARK_WINE_LOG        — Set to 1 for verbose Wine debug output
#
# Fallback ladder controls (see .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md):
#   SPARK_WINE_BACKEND={dxvk|wined3d|null}
#                         — Pin the D3D11 translator layer to a specific rung
#                           of the fallback ladder:
#                             dxvk    = tier 1 (MinGW → Wine → DXVK → Vulkan → Lavapipe)
#                             wined3d = tier 2 (MinGW → Wine → WineD3D → OpenGL → llvmpipe)
#                             null    = tier 3 (MinGW → Wine → SparkEngine NullRHIDevice)
#                           Unset = auto-detect (current behavior: prefer DXVK).
#   SPARK_FORCE_SOFTWARE_GFX=1
#                         — Umbrella knob that forces every graphics layer
#                           onto its CPU rasterizer: sets LIBGL_ALWAYS_SOFTWARE,
#                           GALLIUM_DRIVER=llvmpipe, MESA_LOADER_DRIVER_OVERRIDE,
#                           a Lavapipe-only VK_ICD_FILENAMES, and forces WineD3D
#                           via WINEDLLOVERRIDES when SPARK_WINE_BACKEND is not
#                           already pinning a tier.
#   SPARK_SKIP_WINE=1
#                         — Skip Wine entirely (tier 4). The script locates the
#                           matching native Linux build of the target and runs
#                           it directly, so a developer on a host where Wine is
#                           broken (e.g. under gVisor) still gets engine /
#                           test coverage.
#   SPARK_SKIP_WINE_NATIVE_DIR
#                         — Override the directory where tier-4 looks for the
#                           native Linux build (default: auto-detect from
#                           build/linux-gcc-release/bin, build/linux-gcc-debug/bin,
#                           build/linux-clang-release/bin, build/linux-clang-debug/bin).
#   SPARK_WINE_PROBE      — Pre-flight a known-good PE binary so the script
#                           fails fast instead of spinning in Wine's segv loop.
#   SPARK_WINE_GVISOR_SHIM=1
#                         — LD_PRELOAD tools/gvisor-wine-shim.so to patch
#                           Wine's segv_handler under gVisor's runsc kernel.

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

# SPARK_FORCE_SOFTWARE_GFX — one-shot umbrella that pins every graphics
# layer to its CPU rasterizer so a developer can match what CI sees without
# remembering all five env vars. Overrides any GPU that detect_gpu might
# have picked, so it runs last at the top of main() as well (see below).
if [ "${SPARK_FORCE_SOFTWARE_GFX:-0}" = "1" ]; then
    export LIBGL_ALWAYS_SOFTWARE=1
    export GALLIUM_DRIVER=llvmpipe
    export MESA_LOADER_DRIVER_OVERRIDE=llvmpipe
    for icd in \
        /usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
        /usr/share/vulkan/icd.d/lvp_icd.json; do
        if [ -f "$icd" ]; then
            export VK_ICD_FILENAMES="$icd"
            break
        fi
    done
    # If the user hasn't pinned a Wine backend, default to WineD3D +
    # llvmpipe (tier 2) since that's the most thoroughly-exercised CPU
    # path in Mesa.
    if [ -z "${SPARK_WINE_BACKEND:-}" ]; then
        SPARK_WINE_BACKEND="wined3d"
    fi
fi

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
#   2. `/opt/wine-patched/bin/wine64` if `tools/build-wine-patched.sh` has
#      installed a SparkEngine-patched Wine (gVisor-compat — see
#      `.claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md`)
#   3. `wine64` on $PATH (cleanest, modern distros)
#   4. `/usr/lib/wine/wine64` direct path (bypasses the wrapper)
#   5. `wine` on $PATH as a last resort (may fail with 32-bit .exe)
if [ -n "${WINE:-}" ]; then
    :  # explicit override — honor as-is
elif [ -x "/opt/wine-patched/bin/wine64" ]; then
    WINE="/opt/wine-patched/bin/wine64"
    export WINELOADER="${WINELOADER:-/opt/wine-patched/bin/wine64}"
    info "Using SparkEngine-patched Wine at /opt/wine-patched (gVisor-compat)"
elif [ -x "/opt/wine-patched/bin/wine" ]; then
    WINE="/opt/wine-patched/bin/wine"
    export WINELOADER="${WINELOADER:-/opt/wine-patched/bin/wine}"
    info "Using SparkEngine-patched Wine at /opt/wine-patched (gVisor-compat)"
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
    # Force err+seh,err+virtual for the probe regardless of the ambient
    # WINEDEBUG. The default ambient value is "-all" (to keep the real run
    # quiet), which silently hides the very "Got unexpected trap 0" and
    # "virtual_setup_exception stack overflow" strings we grep for below.
    # Without this override the probe reports success even when Wine is
    # spinning in the segv loop and never executes a single instruction of
    # the target .exe — empirically confirmed on the SparkEngine container
    # harness (same class of signal-handler sandbox as gVisor).
    if ! WINEDEBUG="err+seh,err+virtual" timeout 15 "${WINE}" "${probe}" > "${probe_log}" 2>&1; then
        probe_rc=$?
    fi

    # Detect the two gVisor-class failure modes in order of severity:
    #
    #   1. "Got unexpected trap 0" loop — Wine's segv_handler sees trap_no==0
    #      because the user-mode kernel doesn't populate the x86_64 ucontext
    #      REG_TRAPNO slot. Fixed by tools/gvisor-wine-shim.so (LD_PRELOAD).
    #
    #   2. "virtual_setup_exception stack overflow" — Wine can't build the
    #      Windows-side exception frame because the thread stack layout
    #      differs from Wine's expectations. NOT fixed by the shim; requires
    #      an actual Wine source patch (wine-mirror/wine#62).
    #
    # Reporting the right cause matters because "trap 0" with the shim loaded
    # means the shim isn't taking effect, while "virtual_setup_exception" with
    # the shim loaded means the shim works but the environment is still too
    # restrictive and the user needs tier 2/3/4 instead.
    local saw_trap0=0 saw_vse=0
    grep -q 'segv_handler Got unexpected trap 0' "${probe_log}" && saw_trap0=1
    grep -q 'virtual_setup_exception stack overflow' "${probe_log}" && saw_vse=1
    if [ "${saw_trap0}" = "1" ] || [ "${saw_vse}" = "1" ]; then
        echo "[wine-run] ERROR: Wine cannot execute Windows binaries in this environment." >&2
        if [ "${saw_trap0}" = "1" ] && [ "${saw_vse}" = "0" ]; then
            echo "[wine-run]        Failure mode: 'Got unexpected trap 0' segv_handler loop." >&2
            echo "[wine-run]        Wine's segv_handler reads REG_TRAPNO from the ucontext," >&2
            echo "[wine-run]        but this sandbox's kernel leaves it at 0. The faulting" >&2
            echo "[wine-run]        instruction never gets repaired so the loop is infinite." >&2
            echo "[wine-run]" >&2
            echo "[wine-run]        Fix: build and load the LD_PRELOAD shim:" >&2
            echo "[wine-run]          gcc -shared -fPIC -O2 -o tools/gvisor-wine-shim.so \\" >&2
            echo "[wine-run]              tools/gvisor-wine-shim.c -ldl" >&2
            echo "[wine-run]          SPARK_WINE_GVISOR_SHIM=1 tools/wine-run.sh <exe>" >&2
        elif [ "${saw_vse}" = "1" ] && [ "${saw_trap0}" = "0" ]; then
            echo "[wine-run]        Failure mode: 'virtual_setup_exception stack overflow'." >&2
            echo "[wine-run]        Wine is past the trap_no loop (shim is working) but cannot" >&2
            echo "[wine-run]        build the Windows-side exception frame. This is a second," >&2
            echo "[wine-run]        deeper incompatibility the shim does not cover and which" >&2
            echo "[wine-run]        requires a Wine source patch (wine-mirror/wine#62)." >&2
            echo "[wine-run]" >&2
            echo "[wine-run]        Workaround: fall back to tier 4 of the ladder —" >&2
            echo "[wine-run]          SPARK_SKIP_WINE=1 tools/wine-run.sh <exe>" >&2
            echo "[wine-run]        or set SPARK_WINE_AUTO_TIER4=1 to auto-fall-through." >&2
        else
            echo "[wine-run]        Failure mode: both trap_no loop AND virtual_setup_exception." >&2
            echo "[wine-run]        Shim is not loaded, or is loaded but the loop is still" >&2
            echo "[wine-run]        racing the frame builder. Build the shim and set" >&2
            echo "[wine-run]        SPARK_WINE_GVISOR_SHIM=1; if that still reports" >&2
            echo "[wine-run]        virtual_setup_exception, fall back to tier 4:" >&2
            echo "[wine-run]          SPARK_SKIP_WINE=1 tools/wine-run.sh <exe>" >&2
        fi
        echo "[wine-run]" >&2
        echo "[wine-run]        See .claude/knowledge/wine-gvisor-incompatibility.md" >&2
        echo "[wine-run]        and .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md" >&2
        rm -f "${probe_log}"
        return 2
    fi
    rm -f "${probe_log}"
    return "${probe_rc}"
}

setup_wineprefix() {
    # Pre-populate drive_c/windows/system32 with Wine's shipped Windows DLLs
    # BEFORE running wineboot. Under gVisor-class sandboxes, wineboot's own
    # service startup races with the broken Wine signal delivery and often
    # leaves system32 empty, so a subsequent wine64 run fails with
    # `could not load kernel32.dll, status c0000135`. Hand-populating
    # ensures kernel32/ntdll/user32/etc. are present regardless of whether
    # wineboot itself completes cleanly.
    #
    # The source directory `/usr/lib/x86_64-linux-gnu/wine/x86_64-windows`
    # ships with Ubuntu's libwine package and contains the same set of DLLs
    # wineboot would symlink in. Copying is slightly wasteful vs symlinking
    # but makes the prefix self-contained for archival/relocation.
    local sys32="$WINEPREFIX/drive_c/windows/system32"
    local wine_dll_src="/usr/lib/x86_64-linux-gnu/wine/x86_64-windows"
    if [ ! -f "$sys32/kernel32.dll" ] && [ -d "$wine_dll_src" ]; then
        mkdir -p "$sys32"
        info "Pre-populating $sys32 from $wine_dll_src"
        cp "$wine_dll_src"/*.dll "$sys32/" 2>/dev/null || true
    fi

    # Disable explorer.exe and winemenubuilder.exe during wineboot. Both
    # try to create X11 windows, which fails hard in a headless sandbox
    # and takes the rest of wineboot with it. Setting the override only
    # affects this session — the prefix's registry is untouched so a
    # later graphical run (with a real DISPLAY) still works.
    export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-explorer.exe,winemenubuilder.exe=d}"

    if [ ! -f "$WINEPREFIX/system.reg" ]; then
        info "Initializing Wine prefix at $WINEPREFIX"
        WINEDEBUG=-all "${WINE}" wineboot --init 2>/dev/null || true
        # Wait for wineserver to finish
        "${WINE%wine64}wineserver" --wait 2>/dev/null || true
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
    echo "WINEPREFIX:          $WINEPREFIX"
    echo "VK_ICD_FILES:        ${VK_ICD_FILENAMES:-<system default>}"
    echo "LIBGL_SW:            ${LIBGL_ALWAYS_SOFTWARE:-0}"
    echo "GALLIUM_DRIVER:      ${GALLIUM_DRIVER:-<unset>}"
    echo "DXVK_CACHE:          ${DXVK_STATE_CACHE_PATH:-<not set>}"
    echo ""
    echo "Fallback-ladder knobs:"
    echo "  SPARK_WINE_BACKEND        = ${SPARK_WINE_BACKEND:-<auto>}"
    echo "  SPARK_FORCE_SOFTWARE_GFX  = ${SPARK_FORCE_SOFTWARE_GFX:-0}"
    echo "  SPARK_SKIP_WINE           = ${SPARK_SKIP_WINE:-0}"
    echo "  SPARK_RHI_BACKEND         = ${SPARK_RHI_BACKEND:-<auto>}"
    echo "  SPARK_WINE_GVISOR_SHIM    = ${SPARK_WINE_GVISOR_SHIM:-0}"
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

# Tier-4 fallthrough: given a MinGW .exe path, try to find the equivalent
# native Linux ELF in a sibling build tree. Used by SPARK_SKIP_WINE=1 so a
# developer on a host where Wine is broken (e.g. under gVisor) still gets
# engine / test coverage from the same CMake target.
find_native_linux_equivalent() {
    local exe="$1"
    local basename_noext
    basename_noext="$(basename "$exe" .exe)"
    # Explicit override wins.
    if [ -n "${SPARK_SKIP_WINE_NATIVE_DIR:-}" ]; then
        if [ -x "${SPARK_SKIP_WINE_NATIVE_DIR}/${basename_noext}" ]; then
            echo "${SPARK_SKIP_WINE_NATIVE_DIR}/${basename_noext}"
            return 0
        fi
        return 1
    fi
    # Default probe order — prefer release over debug, GCC over Clang, for
    # consistency with the Linux CI jobs documented in CLAUDE.md.
    for candidate_dir in \
        "${PROJECT_ROOT}/build/linux-gcc-release/bin" \
        "${PROJECT_ROOT}/build/linux-clang-release/bin" \
        "${PROJECT_ROOT}/build/linux-gcc-debug/bin" \
        "${PROJECT_ROOT}/build/linux-clang-debug/bin" \
        "${PROJECT_ROOT}/build/bin"; do
        if [ -x "${candidate_dir}/${basename_noext}" ]; then
            echo "${candidate_dir}/${basename_noext}"
            return 0
        fi
    done
    return 1
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

# SPARK_SKIP_WINE — tier-4 short-circuit. Skip Wine entirely and run the
# matching native Linux build of the target. The tier-4 entry in the
# software-rendering ladder (see
# .claude/knowledge/wine-role-and-fallback-tiers-2026-04-14.md) exists
# precisely for hosts where Wine itself is broken — gVisor's runsc kernel
# being the current motivating case. This is the only rung that doesn't
# exercise the PE / Win32 code path, so it's a diagnostic fallback, not
# the default.
if [ "${SPARK_SKIP_WINE:-0}" = "1" ]; then
    info "SPARK_SKIP_WINE=1 — skipping Wine, looking for native Linux equivalent of ${EXE}"
    NATIVE_EXE="$(find_native_linux_equivalent "$EXE" || true)"
    if [ -z "$NATIVE_EXE" ] || [ ! -x "$NATIVE_EXE" ]; then
        error "SPARK_SKIP_WINE=1 but no native Linux build of '$(basename "$EXE" .exe)' found. \
Build one with: cmake --preset linux-gcc-release && cmake --build build/linux-gcc-release \
OR set SPARK_SKIP_WINE_NATIVE_DIR=<dir> to point at an existing bin/ directory."
    fi
    info "Running native: $NATIVE_EXE $*"
    # Engine-side env-var fallback — no argv control needed. If the caller
    # hasn't already pinned a backend, default to NullRHI so tier-4 comes up
    # headless instead of attempting any GPU ioctls that gVisor blocks.
    export SPARK_RHI_BACKEND="${SPARK_RHI_BACKEND:-null}"
    exec "$NATIVE_EXE" "$@"
fi

# Set up environment
setup_wineprefix

# Try to use real GPU if available (skips Lavapipe) — unless we're forcing
# software rendering via SPARK_FORCE_SOFTWARE_GFX, in which case GPU
# detection would undo our careful Lavapipe-only setup above.
if [ "${SPARK_FORCE_SOFTWARE_GFX:-0}" != "1" ]; then
    detect_gpu || true
fi

# SPARK_WINE_BACKEND — pin the D3D11 translator layer to a specific rung
# of the fallback ladder. Processed AFTER the auto-detect paths so the
# user's choice wins, but BEFORE the probe/launch so WINEDLLOVERRIDES is
# honored for the target .exe.
case "${SPARK_WINE_BACKEND:-}" in
    "")
        # Unset — default behavior: prefer DXVK if present, fall through.
        detect_dxvk && setup_dxvk_cache || true
        detect_vkd3d || true
        ;;
    dxvk|DXVK)
        info "SPARK_WINE_BACKEND=dxvk — pinning tier 1 (DXVK → Vulkan → Lavapipe)"
        if ! detect_dxvk; then
            error "SPARK_WINE_BACKEND=dxvk but DXVK was not found. \
Install with: sudo apt-get install dxvk   (or set DXVK_PATH=<path-to-dxvk/x64>)"
        fi
        setup_dxvk_cache
        detect_vkd3d || true
        export WINEDLLOVERRIDES="d3d11=n,b;dxgi=n,b${WINEDLLOVERRIDES:+;${WINEDLLOVERRIDES}}"
        ;;
    wined3d|WineD3D|WINED3D)
        info "SPARK_WINE_BACKEND=wined3d — pinning tier 2 (WineD3D → OpenGL → llvmpipe)"
        # Do not copy DXVK DLLs into the prefix. Force Wine's built-in
        # implementations of d3d11/dxgi (so any previously-installed DXVK
        # override is bypassed for this run).
        export WINEDLLOVERRIDES="d3d11=b;dxgi=b${WINEDLLOVERRIDES:+;${WINEDLLOVERRIDES}}"
        # llvmpipe is the OpenGL-side software rasterizer WineD3D feeds into.
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER="${GALLIUM_DRIVER:-llvmpipe}"
        export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-llvmpipe}"
        detect_vkd3d || true
        ;;
    null|NULL|none|None)
        info "SPARK_WINE_BACKEND=null — pinning tier 3 (Wine + SparkEngine NullRHIDevice)"
        # Force the engine to select NullRHI regardless of argv. This mirrors
        # action item #5 in wine-role-and-fallback-tiers-2026-04-14.md.
        export SPARK_RHI_BACKEND="${SPARK_RHI_BACKEND:-null}"
        # Skip DXVK/VKD3D copy entirely — they'd be dead code on tier 3.
        ;;
    *)
        error "Unknown SPARK_WINE_BACKEND='${SPARK_WINE_BACKEND}'. Expected one of: dxvk, wined3d, null"
        ;;
esac

# Enable verbose Wine logging if requested
if [ "${SPARK_WINE_LOG:-0}" = "1" ]; then
    export WINEDEBUG="warn+all"
fi

# Optional LD_PRELOAD shim to patch Wine's segv_handler under gVisor. MUST
# be applied BEFORE the probe runs, otherwise the probe sees an environment
# that the user intends to be shimmed but isn't and short-circuits the run.
# Build with: gcc -shared -fPIC -o tools/gvisor-wine-shim.so tools/gvisor-wine-shim.c -ldl
# Enable by setting SPARK_WINE_GVISOR_SHIM=1 (auto-detects tools/gvisor-wine-shim.so).
if [ "${SPARK_WINE_GVISOR_SHIM:-0}" = "1" ]; then
    _shim="${PROJECT_ROOT}/tools/gvisor-wine-shim.so"
    if [ -f "${_shim}" ]; then
        export LD_PRELOAD="${_shim}${LD_PRELOAD:+:${LD_PRELOAD}}"
        info "gVisor shim loaded: ${_shim}"
    else
        info "gVisor shim: not built — build with 'gcc -shared -fPIC -O2 -o ${_shim} ${PROJECT_ROOT}/tools/gvisor-wine-shim.c -ldl'"
    fi
fi

# Optional: probe the Wine environment with a pre-built hello-world .exe
# before running the real target, so sandboxed / gVisor environments fail
# fast with a clear diagnostic instead of spinning in Wine's segv loop.
# Enable by setting SPARK_WINE_PROBE=/path/to/hello.exe
if [ -n "${SPARK_WINE_PROBE:-}" ]; then
    if ! probe_wine_environment "${SPARK_WINE_PROBE}"; then
        # Probe failed — offer the tier-4 fallthrough automatically if the
        # user has set SPARK_WINE_AUTO_TIER4=1. This is the pragmatic thing
        # to do in CI: if Wine is broken in this specific environment and
        # the user has pre-opted-in to tier 4, transparently skip Wine and
        # run the native Linux equivalent. Without the opt-in we still
        # exit 2 so a human-driven run sees the diagnostic and chooses.
        if [ "${SPARK_WINE_AUTO_TIER4:-0}" = "1" ]; then
            info "SPARK_WINE_AUTO_TIER4=1 — probe failed, attempting tier-4 fallthrough"
            NATIVE_EXE="$(find_native_linux_equivalent "$EXE" || true)"
            if [ -n "$NATIVE_EXE" ] && [ -x "$NATIVE_EXE" ]; then
                info "Running native: $NATIVE_EXE $*"
                export SPARK_RHI_BACKEND="${SPARK_RHI_BACKEND:-null}"
                exec "$NATIVE_EXE" "$@"
            fi
            info "Tier-4 fallthrough requested but no native Linux build found — exiting with probe failure"
        fi
        exit 2
    fi
fi

info "Running: ${WINE} $EXE $*"
info "  Vulkan ICD: ${VK_ICD_FILENAMES:-<system default>}"
info "  DXVK cache: ${DXVK_STATE_CACHE_PATH:-<disabled>}"

# Run under Wine
exec "${WINE}" "$EXE" "$@"
