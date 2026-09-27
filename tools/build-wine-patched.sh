#!/usr/bin/env bash
# build-wine-patched.sh — Build Wine 9.0 (or newer) from source with the three
# SparkEngine gVisor compat patches applied, install to /opt/wine-patched, and
# verify with a hello-world reproducer.
#
# Use this on a host where the LD_PRELOAD `tools/gvisor-wine-shim.so` cannot
# fully escape the gVisor + Wine race condition documented in
# `wiki/advanced/Wine-Role-and-Fallback-Tiers.md`. The patched Wine fixes
# all three failure modes natively in Wine source — see
# `wiki/advanced/Wine-Role-and-Fallback-Tiers.md` for the full
# diagnosis and `docs/wine-upstream/0001..0003-*.patch` for the patches.
#
# Requirements:
#   - Internet access to www.unicode.org (for tools/make_unicode) and the
#     Khronos Vulkan/OpenGL XML registries (for dlls/winevulkan/make_vulkan
#     and dlls/opengl32/make_opengl). gVisor sandboxes typically block these.
#   - apt-get build-dep wine succeeds (~600 MB of build deps)
#   - ~5 GB free disk and ~30 minutes wall clock on a fast machine
#
# Usage:
#   sudo tools/build-wine-patched.sh                # build, install, verify
#   sudo tools/build-wine-patched.sh --build-only   # don't run the verifier
#   tools/build-wine-patched.sh --check             # report whether patched
#                                                   #   Wine is already installed
#
# Environment variables:
#   WINE_VERSION   — Wine source tarball version (default: 9.0). The patches
#                    in docs/wine-upstream/ apply cleanly to 9.0; newer Wines
#                    may need a refreshed patch series.
#   PREFIX         — Install prefix (default: /opt/wine-patched)
#   BUILD_DIR      — Build directory (default: a fresh private mktemp -d directory).
#                    A directory you supply must be owned by you, not a symlink,
#                    not group/world-writable, and reached through no writable
#                    non-sticky ancestor: this script runs its contents as root.
#   JOBS           — make -j parallelism (default: $(nproc))

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WINE_VERSION="${WINE_VERSION:-9.0}"
PREFIX="${PREFIX:-/opt/wine-patched}"
BUILD_DIR="${BUILD_DIR:-}"
JOBS="${JOBS:-$(nproc)}"

PATCH_DIR="$PROJECT_ROOT/docs/wine-upstream"

# Three patches, in apply order. Patches 1 and 2 apply cleanly to Wine 9.0
# upstream tarball. Patch 3 (the wrgsbase set_gs_base helper from PR #63)
# was authored against Wine 11.x and the function names + line numbers have
# drifted; on 9.0 we apply the equivalent fix as a single inline patch to
# the static `arch_prctl` helper at the top of signal_x86_64.c, which catches
# every call site automatically (init_syscall_frame, init_handler,
# check_invalid_gsbase) without needing the helper-extraction refactor.
PATCH_1="$PATCH_DIR/0001-ntdll-fall-back-to-siginfo-for-trap-code.patch"
PATCH_2="$PATCH_DIR/0002-ntdll-refresh-stack-info-from-pthread-under-gVisor.patch"

# ============================================================================
# Helpers
# ============================================================================

info() { echo "[wine-build] $*"; }
warn() { echo "[wine-build] WARN: $*" >&2; }
fail() { echo "[wine-build] ERROR: $*" >&2; exit 1; }

# Everything under the build directory is later executed as root (generators, configure, make, make install),
# so nobody but the invoking user may be able to write to it or swap it out. A shared, predictable path such as
# /tmp/wine-build lets any local user pre-create it and plant or race-edit the sources.
#
# The caller must pass the canonical path (resolve it with `cd -P` first) and use only that path afterwards. A
# symlink anywhere in the path is rejected, because whoever owns it can re-point it after the check. Every
# ancestor must be owned by root or the invoking user: the owner of a directory can always rename entries in it,
# sticky bit or not, and so could move the checked directory away and substitute their own.
require_private_dir() {
    local dir="$1"
    [[ -d "$dir" && ! -L "$dir" ]] || fail "build directory $dir is not a real directory"
    local canonical
    canonical="$(cd -P -- "$dir" && pwd -P)" || fail "build directory $dir cannot be resolved"
    [[ "$canonical" == "$dir" ]] || fail "build directory $dir is not canonical (resolves to $canonical)"
    [[ "$(stat -c %u -- "$dir")" == "$EUID" ]] || fail "build directory $dir is not owned by uid $EUID"
    (( ( 8#$(stat -c %a -- "$dir") & 8#022 ) == 0 )) || fail "build directory $dir is group- or world-writable"

    # An ancestor that others can write to without the sticky bit lets them rename the directory away and
    # substitute their own.
    local ancestor="$dir"
    while [[ "$ancestor" != "/" ]]; do
        ancestor="$(dirname -- "$ancestor")"
        local owner
        owner="$(stat -c %u -- "$ancestor")"
        if [[ "$owner" != "0" && "$owner" != "$EUID" ]]; then
            fail "build directory ancestor $ancestor is owned by uid $owner, not root or uid $EUID"
        fi
        local mode
        mode=$(( 8#$(stat -c %a -- "$ancestor") ))
        if (( (mode & 8#022) != 0 && (mode & 8#1000) == 0 )); then
            fail "build directory ancestor $ancestor is writable by others and not sticky"
        fi
    done
}

# ============================================================================
# Check mode — report whether patched Wine is already installed
# ============================================================================

if [[ "${1:-}" == "--check" ]]; then
    if [[ -x "$PREFIX/bin/wine" ]] || [[ -x "$PREFIX/bin/wine64" ]]; then
        echo "[OK]   Patched Wine present at $PREFIX"
        "$PREFIX/bin/wine" --version 2>/dev/null || \
            "$PREFIX/bin/wine64" --version 2>/dev/null || true
        exit 0
    else
        echo "[MISS] No patched Wine at $PREFIX"
        exit 1
    fi
fi

BUILD_ONLY=0
[[ "${1:-}" == "--build-only" ]] && BUILD_ONLY=1

# ============================================================================
# Step 1 — Install build dependencies
# ============================================================================

info "Installing Wine build dependencies (this may take several minutes)..."
if ! command -v dpkg-source &>/dev/null || ! dpkg -l libfreetype-dev &>/dev/null; then
    if [[ $EUID -ne 0 ]]; then
        fail "Need root to install build deps. Re-run with sudo."
    fi
    # Enable deb-src if not already present
    if ! grep -q "^Types: deb deb-src" /etc/apt/sources.list.d/ubuntu.sources 2>/dev/null; then
        sed -i 's|^Types: deb$|Types: deb deb-src|' /etc/apt/sources.list.d/ubuntu.sources
        apt-get update
    fi
    apt-get build-dep -y wine
    apt-get install -y libfreetype-dev libpcap0.8-dev libxml2-dev flex bison \
                       autoconf automake build-essential perl python3
fi

# ============================================================================
# Step 2 — Fetch + extract Wine source
# ============================================================================

info "Fetching Wine $WINE_VERSION source..."
if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/wine-build.XXXXXXXX")"
elif [[ ! -e "$BUILD_DIR" && ! -L "$BUILD_DIR" ]]; then
    mkdir -m 0700 -- "$BUILD_DIR"
fi
# Canonicalise once and use only the canonical path from here on (cd, verify dir, rm -rf, the executed .exe),
# so a symlinked component cannot be re-pointed between the check and its use.
BUILD_DIR="$(cd -P -- "$BUILD_DIR" && pwd -P)" || fail "cannot resolve build directory $BUILD_DIR"
require_private_dir "$BUILD_DIR"
cd -- "$BUILD_DIR"

# The source comes only from `apt-get source`, which verifies every file it fetches (or reuses in this directory)
# against the signed Sources index. No tarball found lying around elsewhere is ever trusted.
apt-get source --download-only wine || fail "apt-get source wine failed (is deb-src enabled?)"
shopt -s nullglob
# Both entries are globs ([x] included) so nullglob drops whichever form apt-get did not produce.
WINE_TARBALLS=( "wine_${WINE_VERSION}".orig.tar.[x]z "wine_${WINE_VERSION}"~*.orig.tar.xz )
shopt -u nullglob
[[ ${#WINE_TARBALLS[@]} -eq 1 ]] || \
    fail "expected exactly one verified wine_${WINE_VERSION} orig tarball from apt-get source, found ${#WINE_TARBALLS[@]}"

rm -rf "wine-${WINE_VERSION}"
tar -xJf "${WINE_TARBALLS[0]}"
cd "wine-${WINE_VERSION}"

# ============================================================================
# Step 3 — Apply patches
# ============================================================================

info "Applying SparkEngine gVisor-compat patches..."
patch -p1 < "$PATCH_1" || fail "Patch 1 failed: $PATCH_1"
patch -p1 < "$PATCH_2" || fail "Patch 2 failed: $PATCH_2"

# Patch 3 inline — replace the static arch_prctl helper with the
# wrgsbase-fallback version. This is the Wine 9.0 equivalent of the
# `set_gs_base()` extraction in upstream PR #63.
SIG_FILE="dlls/ntdll/unix/signal_x86_64.c"
if grep -q "static inline int arch_prctl( int func, void \*ptr ) { return syscall( __NR_arch_prctl, func, ptr ); }" "$SIG_FILE"; then
    info "Patching arch_prctl helper inline (Wine 9.0 path)..."
    python3 -c "
import re, sys
src = open('$SIG_FILE').read()
old = 'static inline int arch_prctl( int func, void *ptr ) { return syscall( __NR_arch_prctl, func, ptr ); }'
new = '''static inline int arch_prctl( int func, void *ptr )
{
    int rc = syscall( __NR_arch_prctl, func, ptr );
    if (func == ARCH_SET_GS)
    {
        unsigned long probe = 0;
        __asm__ volatile( \"movq %%gs:0x30, %0\" : \"=r\"(probe) );
        if (probe != (unsigned long)ptr)
        {
            __asm__ volatile( \"wrgsbase %0\" :: \"r\"((unsigned long)ptr) );
            return 0;
        }
    }
    return rc;
}'''
open('$SIG_FILE', 'w').write(src.replace(old, new))
"
else
    warn "arch_prctl helper not found in expected form; skipping inline patch"
fi

# ============================================================================
# Step 4 — Generate auto-generated headers (Vulkan, OpenGL, server protocol,
# Unicode tables). The Wine build expects these to exist before configure
# runs makedep, but the upstream tarball ships them stripped.
# ============================================================================

info "Generating auto-generated headers..."
perl tools/make_requests
[[ -f tools/make_unicode ]] && perl tools/make_unicode || \
    warn "make_unicode failed (needs network access to www.unicode.org)"
(cd dlls/winevulkan && python3 make_vulkan -x vk-*.xml 2>/dev/null) || \
    warn "make_vulkan failed (needs network access to khronos.org)"
(cd dlls/opengl32 && perl make_opengl 2>/dev/null) || \
    warn "make_opengl failed (needs network access to khronos.org)"

# ============================================================================
# Step 5 — Configure + build
# ============================================================================

info "Configuring Wine (installing to $PREFIX)..."
./configure --prefix="$PREFIX" --enable-win64 --disable-tests \
    --without-x --without-freetype --without-gstreamer --without-mingw \
    || fail "configure failed"

info "Building Wine with -j$JOBS (this takes 15-60 minutes)..."
make -j"$JOBS" || fail "make failed"

info "Installing to $PREFIX..."
if [[ $EUID -eq 0 ]]; then
    make install
else
    sudo make install
fi

# ============================================================================
# Step 6 — Verify with hello-world reproducer
# ============================================================================

if [[ $BUILD_ONLY -eq 1 ]]; then
    info "Build complete. Skipping verification (--build-only)."
    exit 0
fi

info "Verifying patched Wine with hello-world reproducer..."
# Inside the private build directory, never at a fixed /tmp path another user could pre-plant.
VERIFY_DIR="$BUILD_DIR/verify"
rm -rf "$VERIFY_DIR"
mkdir -m 0700 "$VERIFY_DIR"
cat > "$VERIFY_DIR/spark-wine-hello.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("hello from patched wine\n"); return 42; }
EOF
x86_64-w64-mingw32-gcc "$VERIFY_DIR/spark-wine-hello.c" -o "$VERIFY_DIR/spark-wine-hello.exe" || \
    fail "MinGW cross-compile failed"

RC=0
WINEPREFIX="$VERIFY_DIR/wineprefix" "$PREFIX/bin/wine64" "$VERIFY_DIR/spark-wine-hello.exe" || RC=$?
if [[ $RC -eq 42 ]]; then
    info "VERIFIED — patched Wine runs guest binaries to completion (rc=$RC)"
else
    warn "Patched Wine ran but rc=$RC (expected 42)"
    exit 1
fi
