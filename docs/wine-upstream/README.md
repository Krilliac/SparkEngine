# Wine upstream patches for gVisor/UMH compatibility

This directory contains two patches against Wine that fix bugs
`SparkEngine` hit running MinGW-cross-compiled binaries under Wine
inside a gVisor-backed sandbox. The root causes are documented in
[Wine Role and Fallback Tiers](../../wiki/advanced/Wine-Role-and-Fallback-Tiers.md).

Both patches target **Wine 9.0** (tag `wine-9.0`, commit hash
visible via `git describe` in the upstream tree). The line numbers
and context in the diffs were produced against that tag; they should
apply cleanly to 9.0 and likely apply with small offset adjustments
to 9.x master.

## Patches

| # | File | Touches | Lines |
|---|------|---------|-------|
| 1 | `0001-ntdll-fall-back-to-siginfo-for-trap-code.patch`        | `dlls/ntdll/unix/signal_x86_64.c` | +47 / −3 |
| 2 | `0002-ntdll-refresh-stack-info-from-pthread-under-gVisor.patch` | `dlls/ntdll/unix/virtual.c`       | +65 / −11 |

Each patch is self-contained and submittable independently, but they
must both be applied to fully run Wine under gVisor. Applying only
patch 1 takes the engine past the `Got unexpected trap 0` infinite
loop and exposes the stack-overflow bug that patch 2 fixes. Applying
only patch 2 has no observable effect under gVisor because patch 1's
bug fires first.

## The bugs

### Bug 1 — `segv_handler` trusts `REG_TRAPNO == 0` as a real trap

**File:** `dlls/ntdll/unix/signal_x86_64.c`
**Functions:** `segv_handler`, `trap_handler`

Wine's x86_64 signal handlers read the hardware trap number via
`TRAP_sig(ucontext) == ucontext->uc_mcontext.gregs[REG_TRAPNO]`. When
that field is zero — which happens on user-mode Linux kernels that
synthesise signals (gVisor, Firecracker, qemu-user in some modes) —
the `switch` falls through to `default`, prints `Got unexpected
trap 0`, and returns without repairing the fault. The faulting
instruction re-executes immediately, producing an infinite loop
thousands of iterations per second.

**Evidence this is the only thing wrong at the OS layer:** an LD_PRELOAD
shim (`tools/gvisor-wine-shim.c`) that does nothing except force
`gregs[REG_TRAPNO] = 14` and `gregs[REG_ERR] = 0x6` before chaining
to Wine's real handler is sufficient to make Wine proceed past the
loop.

**Fix:** `siginfo->si_code` is always populated by the common kernel
signal path, even when the ucontext trap fields are not. Add a
`get_signal_trap_code()` helper that reads `TRAP_sig()` first and
falls back to deriving the trap from `si_code` only when the ucontext
said zero. Use the helper in `segv_handler` and `trap_handler`. On
bare metal Linux the helper returns the unchanged hardware value and
the fallback is never taken — so the fix is invisible except on the
environments it was written for.

### Bug 2 — `virtual_setup_exception` spurious stack overflow

**File:** `dlls/ntdll/unix/virtual.c`
**Function:** `virtual_setup_exception`

After patch 1 routes page faults to the correct handler, the next
issue surfaces: when Wine tries to write a Windows exception frame
onto the guest thread stack, `virtual_setup_exception` range-checks
the destination against `DeallocationStack`/`StackBase` cached in the
TEB at thread creation. If the proposed write would land in the first
4 KB of the cached mapping (the guard region), Wine calls
`abort_thread(1)` with a `stack overflow N bytes` error and kills
the thread.

On gVisor and similar environments the pthread stack the kernel
actually allocated does not exactly coincide with the mapping Wine
cached in the TEB. The cached bounds are too narrow, so the check
rejects a frame that would in fact land in genuinely mapped,
writable memory.

**Fix:** before declaring the overflow unrecoverable, query the real
pthread stack via `pthread_getattr_np()` + `pthread_attr_getstack()`.
If the kernel reports a lower start address than the TEB cached,
widen `stack_info->start` and retry the range check. On bare metal
the pthread start and the TEB start agree, so the retry never fires
and behavior is unchanged. Only the gVisor case is affected.

Guarded by `defined(__linux__) && defined(__GLIBC__)` because
`pthread_getattr_np` is a glibc extension; on other libc and on
non-Linux targets the helper is a no-op.

## Applying

In your Wine fork (checked out at the `wine-9.0` tag or current master):

```bash
cd /path/to/your/wine-fork
git checkout -b fix/gvisor-compat wine-9.0
git am /path/to/SparkEngine/docs/wine-upstream/0001-ntdll-fall-back-to-siginfo-for-trap-code.patch
git am /path/to/SparkEngine/docs/wine-upstream/0002-ntdll-refresh-stack-info-from-pthread-under-gVisor.patch
```

If `git am` rejects because you're on master rather than the 9.0 tag,
use `patch -p1` and re-commit manually — both patches are small and
self-explanatory:

```bash
patch -p1 < /path/to/SparkEngine/docs/wine-upstream/0001-ntdll-fall-back-to-siginfo-for-trap-code.patch
patch -p1 < /path/to/SparkEngine/docs/wine-upstream/0002-ntdll-refresh-stack-info-from-pthread-under-gVisor.patch
git add dlls/ntdll/unix/signal_x86_64.c dlls/ntdll/unix/virtual.c
git commit
```

## Building a patched Wine without touching the distro install

Put the patched `wine64` in its own prefix so the distro Wine stays
available for everything else:

```bash
sudo apt-get install -y flex bison libncurses-dev libx11-dev  # minimal deps
cd /path/to/your/wine-fork
mkdir -p build && cd build
../configure --prefix=$HOME/wine-patched \
             --enable-win64 \
             --disable-tests \
             --without-freetype \
             --without-gstreamer
make -j$(nproc)
make install
```

Then run SparkEngine's test suite against the patched binary by
pointing the runner at it:

```bash
cd /path/to/SparkEngine
WINELOADER=$HOME/wine-patched/bin/wine64 \
    SPARK_WINE_GVISOR_SHIM=0 \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe
```

`SPARK_WINE_GVISOR_SHIM=0` is the important part — it disables the
LD_PRELOAD workaround in `tools/gvisor-wine-shim.c` so that if the
patched Wine still loops at trap 0 or dies on stack overflow, you
know the patch isn't complete.

## Verification

### Minimal smoke test (patch 1)

```c
/* hello.c — cross-compile with: x86_64-w64-mingw32-gcc -municode hello.c -o hello.exe -mwindows */
#include <windows.h>
int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, PWSTR cmd, int show) { return 0; }
```

```bash
WINELOADER=$HOME/wine-patched/bin/wine64 wine64 hello.exe; echo "rc=$?"
```

**Expected before patch 1:** `err:seh:segv_handler Got unexpected trap 0`
spinning indefinitely.
**Expected after patch 1:** process returns, `rc=0` (or `rc=255` on a
separate unrelated Wine quirk around redirected stdio).

### Full smoke test (patches 1 + 2)

```bash
WINELOADER=$HOME/wine-patched/bin/wine64 \
    SPARK_WINE_GVISOR_SHIM=0 \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe -test-frames 3
```

**Expected before patch 2:** even with patch 1 applied, the engine
prints `err:virtual:virtual_setup_exception stack overflow 64 bytes
...` on the first raised exception and exits.
**Expected after patch 2:** engine boots, runs 3 frames, prints a
normal shutdown summary, exits cleanly. Under `fixme`-level Wine
debug output you may see one `WARN:virtual:virtual_setup_exception
stack_info start widened from TEB value to pthread value` line the
first time a thread handles an exception — that is the patch working
as intended, not an error.

### Regression test on bare metal

Run the same commands on a conventional Linux host (laptop, VM,
`docker run --runtime=runc`, a CI runner) to confirm the patches do
not change behavior outside gVisor. On bare metal:

 * Patch 1's `get_signal_trap_code()` always returns the non-zero
   hardware `TRAP_sig()` value, so the fallback switch is never
   reached.
 * Patch 2's `refresh_stack_info_from_pthread()` is only called when
   the cached check is about to fail, and the pthread bounds are
   expected to match the TEB bounds, so `stack_info->start` is never
   widened.

The only observable difference in normal use is the extended
"Got unexpected trap %u (hw %ld)" log format in the `segv_handler`
default case — but that path only runs on genuinely unknown trap
types, which shouldn't happen in healthy operation.

## Submitting upstream

Wine upstream prefers patches to `wine-devel@winehq.org` (mailing
list). These two patches are small enough to post inline with
`git send-email`. Suggested subjects:

  * `[PATCH 1/2] ntdll: fall back to siginfo for trap code when REG_TRAPNO is zero`
  * `[PATCH 2/2] ntdll: refresh stack_info from pthread bounds before declaring overflow`

Cover letter / cover comment should mention:

  * The two-bug chain (patch 1 exposes bug 2).
  * Reproduction requires a user-mode Linux kernel (gVisor is the
    easiest to get: `docker run --runtime=runsc ubuntu`).
  * Bare-metal regression testing: patches are no-ops on normal Linux.
  * Reference SparkEngine's LD_PRELOAD shim as prior-art evidence that
    patch 1's change is sufficient:
    <https://github.com/Krilliac/SparkEngine/blob/claude/fix-bugs-stability-alRBr/tools/gvisor-wine-shim.c>

## Open questions / future work

Patch 2's diagnosis (pthread stack at a lower address than the TEB
cached) was inferred from the "stack overflow 64 bytes" error
message reported in SparkEngine's test run. It has not yet been
confirmed against a live debugger attached to Wine inside gVisor.
The fallback path is defensive: if the pthread bounds turn out to
*also* be too narrow, the retry check will fail and we fall through
to the original `abort_thread(1)` — so at worst this patch is a
no-op in cases it can't help. But a second diagnostic pass on a real
gVisor reproducer would be worth doing before final submission, to
either strengthen the cover letter or refine the fix.

If the underlying issue turns out to be in `virtual_alloc_thread_stack`
(thread stack bookkeeping diverging from the actual mapping at thread
creation time, not at exception time), the correct upstream fix would
be to populate `stack->DeallocationStack`/`StackBase` from
`pthread_attr_getstack()` at creation time instead of from Wine's
own `map_view()`. Patch 2 would then become unnecessary. That is a
larger, more invasive change and is deliberately not attempted here.

## References

 * Wine source browsed: `dlls/ntdll/unix/signal_x86_64.c` (segv_handler,
   trap_handler, get_signal_trap_code helper) and `dlls/ntdll/unix/virtual.c`
   (virtual_setup_exception, new refresh_stack_info_from_pthread helper).
 * Root-cause analysis: [Wine Role and Fallback Tiers](../../wiki/advanced/Wine-Role-and-Fallback-Tiers.md)
   (on the `claude/fix-bugs-stability-alRBr` branch / PR #470).
 * LD_PRELOAD proof-of-concept for bug 1: [`tools/gvisor-wine-shim.c`](../../tools/gvisor-wine-shim.c)
   (same PR).
 * gVisor platform signal emulation: <https://github.com/google/gvisor/tree/master/pkg/sentry/platform>
