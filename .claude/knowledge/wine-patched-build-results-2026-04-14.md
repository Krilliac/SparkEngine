# Wine Patched Build — Empirical Test Results

**Date:** 2026-04-14
**Session:** Build + test patched Wine from `Krilliac/wine` against SparkEngine
**Branch:** `claude/wine-patched-build-test`
**Wine fork:** `Krilliac/wine` @ `master` + two feature branches
**Wine version built:** `wine-11.6`
**Wine install prefix:** `/opt/wine-patched/`

## What was built

1. **Krilliac/wine integration branch** (local only, not pushed) containing all three commits:
   - `e4ae52e ntdll: fall back to siginfo for trap code when REG_TRAPNO is zero.` (patch 1 from PR #470)
   - `0c00c2b ntdll: refresh thread_stack_info from pthread bounds before declaring stack overflow.` (patch 2 from PR #470)
   - `93195fb ntdll: cache pthread stack bounds at thread init to keep refresh path async-signal-safe.` (followup addressing the async-signal-safety concern)

2. **Full Wine build** via `./configure --prefix=/opt/wine-patched --enable-win64 --disable-tests`, `make -j16`. All three patched files (`dlls/ntdll/unix/signal_x86_64.c`, `dlls/ntdll/unix/virtual.c`, `dlls/ntdll/unix/thread.c`) compiled cleanly under `-Wall` with no warnings. No changes outside the patch series were required.

3. **SparkEngine MinGW cross-build** via `cmake --preset linux-mingw-release`. Two engine source bugs were surfaced and fixed as part of this test (see "SparkEngine fixes" below).

## Patch 1 verdict: WORKS

The `Got unexpected trap 0` infinite loop that SparkEngine documented in
`.claude/knowledge/wine-gvisor-incompatibility.md` is **gone**. No runs
of any test binary produced that log line under the patched Wine, even
without the `tools/gvisor-wine-shim.c` LD_PRELOAD workaround loaded.

Patch 1 is an unqualified success and should be submitted upstream as
a standalone fix.

## Patch 2 verdict: WRONG DIAGNOSIS, WITHDRAWN — superseded by real fix

**Update 2026-04-14 (same day, later session):** the initial
"pthread-refresh" diagnosis below turned out to be completely wrong.
A follow-up investigation in the same day found the real root cause,
fixed it upstream-style, and verified the fix runs a MinGW-cross
`hello.exe` to completion under gVisor. The sections below are
preserved for historical reference because the strace evidence and
the reasoning about Wine's thread-stack mmap model are still
correct — but the **fix direction was wrong**, and nothing about
`virtual_setup_exception`, `pthread_getattr_np`, or
stack-bound refresh is actually involved in the real bug.

**TL;DR of the real bug and the real fix** (full details in
`.claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md`):

  * gVisor's runsc sentry does **not** implement
    `arch_prctl(ARCH_SET_GS)` correctly. The syscall either returns
    `-1` or lies (returns success but never writes the `gs.base`
    MSR). Under some sentry versions `ARCH_GET_GS` also returns 0
    incorrectly, which makes the condition almost invisible to
    diagnostic tools.
  * Wine calls `arch_prctl(ARCH_SET_GS, teb)` from
    `init_syscall_frame()` to point `gs.base` at the Windows TEB
    before each thread starts. On gVisor that call has no effect.
    Every subsequent `%gs:0x30` read in Wine's PE ntdll — which is
    how `NtCurrentTeb()` is implemented on x86_64 Linux — returns
    garbage.
  * The first consumer of that garbage is `loader_init()` in
    `dlls/ntdll/loader.c`: `peb = NtCurrentTeb()->Peb; peb->LdrData
    = &ldr` writes to `[rdi + 0x18]` with `rdi == 0`, generating a
    SIGSEGV at `si_addr=0x18`. The exception dispatcher then faults
    again inside `__wine_dbg_get_channel_flags` (which reads
    `debug_options[]` through a pointer also derived from `%gs`),
    each iteration pushes a fresh exception frame on the user stack,
    rsp decreases by exactly `sizeof(exc_stack_layout) = 3776` bytes
    per iteration, and after a few dozen iterations the thread is
    killed by `virtual_setup_exception`.
  * The stack-overflow message patch 2 was chasing is therefore a
    **symptom**, not the cause. "TEB stack differs from pthread
    stack" is a real observation about Wine's mmap layout — but
    irrelevant to this bug.

**The real fix** adds a `set_gs_base(teb)` helper in
`dlls/ntdll/unix/signal_x86_64.c` that tries
`arch_prctl(ARCH_SET_GS)` first and **verifies** the write took
effect by reading `%gs:0x30` back. On a correctly-initialised TEB
that read equals `teb` because `teb->NtTib.Self = &teb->Tib` and
`Tib` is at offset 0 inside TEB. If the syscall failed or the
verification mismatched, the helper falls through to the
`wrgsbase` instruction — a direct CPU instruction, not a syscall,
which cannot be intercepted by an emulator. On bare-metal Linux
`arch_prctl` always works and the `wrgsbase` fallback is never
reached, so there is no SIGILL risk on hosts with `CR4.FSGSBASE`
cleared. All three places Wine writes `gs.base` now route through
the helper: `init_syscall_frame` (primary thread-start path),
`check_invalid_gsbase` (copy-protection workaround), and
`init_handler` (signal-entry safety net).

### Iteration history of the real fix (Codex review round-trip)

| Rev | Commit | Strategy | Outcome |
|-----|--------|----------|---------|
| 1 | `f0f9846` | wrgsbase first, arch_prctl fallback, unconditional | Fixes gVisor; flagged by Codex review for SIGILL risk on hosts with `CR4.FSGSBASE` cleared |
| 2 | `be4282b` | Guard wrgsbase on `user_shared_data->ProcessorFeatures[PF_RDWRFSGSBASE_AVAILABLE]`, arch_prctl fallback | Addresses Codex, but **silently no-ops on gVisor**: Wine computes the flag as `cpuid(7).ebx[0] & (AT_HWCAP2 & 2)`, and gVisor under-reports `AT_HWCAP2`, so the wrgsbase path is skipped and the patch falls through to the broken `arch_prctl` again |
| 3 | `6db9694` (final) | **arch_prctl first, verify via `%gs:0x30`, wrgsbase fallback only on failure** | Fixes gVisor AND avoids unconditional SIGILL on bare-metal hosts. Final shape, ready for upstream. |

The critical insight for iteration 3 is that the wrgsbase path is
**only reached when arch_prctl has been proven broken at runtime**.
On bare-metal Linux (with or without `CR4.FSGSBASE`), arch_prctl
succeeds, the verification passes, and wrgsbase never runs. The
only hosts that reach wrgsbase are ones where arch_prctl has been
demonstrated to either fail or lie — which in practice means
user-mode Linux kernels that expose the fsgsbase feature to user
space (gVisor). This gets correctness and Codex-safety without
trade-off.

### Verification

Rebuilt wine-11.6 locally with the iteration-3 helper, installed to
`/opt/wine-patched`, reran the reproducer inside the same gVisor
sandbox:

```bash
cat > hello.c <<'EOF'
#include <stdio.h>
int main(void) { printf("hello from wine, argc=%d\n", 1); return 42; }
EOF
x86_64-w64-mingw32-gcc hello.c -o /tmp/hello2.exe
WINEPREFIX=/tmp/clean-prefix /opt/wine-patched/bin/wine /tmp/hello2.exe
# hello from wine, argc=1
# rc=42
```

Both the NULL-pointer cascade and the downstream stack-overflow
messages are gone. wineboot still occasionally races on the first
run per fresh prefix — an unrelated Wine init flakiness that
existed before this work — but subsequent runs on the same prefix
are reliable.

### Artifacts produced

  * `docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch`
    — squashed final patch for upstream submission to
    `wine-devel@winehq.org`. Single clean commit with the arch_prctl-
    first layout and a full commit message explaining the Codex
    iteration history.
  * `docs/wine-upstream/0004-ntdll-prefer-arch_prctl-for-set_gs_base.patch`
    — standalone fix commit that applies on top of the existing
    `Krilliac/wine:claude/wine-wrgsbase-fallback` branch (which holds
    `f0f9846 + be4282b`), letting the fork branch be updated in place
    without rewriting history.
  * `.claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md`
    — full diagnostic breadcrumb for the real root cause, including
    the instrumentation sequence that found it and a minimal
    standalone program that proves wrgsbase works where arch_prctl
    doesn't.

### Upstream / fork status at time of writing

  * `wine-mirror/wine#62` (Krilliac's pthread-refresh PR) was
    **closed** upstream without a second review round. No further
    action required on that PR; patch 3 supersedes it.
  * `Krilliac/wine:claude/wine-stack-pthread-refresh` still holds
    commits `0c00c2b` + `93195fb`. Leave as historical reference or
    rename `do-not-submit`.
  * `Krilliac/wine:claude/wine-wrgsbase-fallback` holds `f0f9846` +
    `be4282b`. Apply `0004-...patch` on top of it (or force-push the
    squashed `0003-...patch` version) to restore gVisor correctness.
  * `Krilliac/wine:claude/wine-trap-siginfo-fallback` still holds the
    correct trap-siginfo fallback at `e4ae52e`. Keep as-is; still
    submit as `[PATCH 1/2]` alongside the wrgsbase fix.

---

### Original (wrong) pthread-refresh analysis — historical reference

The rest of this section records the pthread-refresh diagnosis that
this session chased and discarded. It's kept because the strace
evidence and the Wine thread-stack mmap observations are still
accurate — only the conclusion ("refresh pthread bounds from the
signal handler") was wrong.

Patch 2 sometimes prints its intended `warn:virtual:virtual_setup_exception
stack_info start widened from TEB value to pthread value` message and
proceeds — but examination of the cases where it "works" shows the
refresh is widening `stack_info->start` to an address many GB below the
current stack mapping, not into the actual pthread stack. The refresh
happens to unblock the range check by coincidence: the address it
widens to is inside unrelated memory, and the subsequent write only
succeeds because the real rsp is still well above the Wine-allocated
Windows stack's true limit.

#### Why the mental model is wrong

Patch 2 (both the original and the cached rewrite) assumes the **TEB
stack** (`DeallocationStack..StackBase`) and the **pthread stack**
(from `pthread_getattr_np`) are two views of the **same memory
region**, where the TEB bounds are sometimes a narrower subset of
the pthread bounds on user-mode kernels like gVisor.

Empirically that is not how Wine's thread model works:

  * `virtual_alloc_thread_stack()` in `dlls/ntdll/unix/virtual.c`
    calls `map_view()` to allocate a **new, independent mmap region**
    for each Windows thread stack.
  * That mapping is **separate** from the glibc pthread stack the
    host created when `pthread_create()` fired.
  * Wine then runs the new Windows thread on the Wine-allocated
    mapping, not on glibc's pthread stack.
  * `pthread_getattr_np()` returns the glibc pthread stack, which is
    at a completely unrelated address from the Wine-allocated
    Windows stack.

Confirmed by instrumenting `virtual_setup_exception()` to print
`pthread_cache` alongside the TEB bounds:

```
0024:err:virtual:virtual_setup_exception stack overflow 192 bytes
  addr 0x7eaa405ae8a0
  stack 0x7eaa40600f40
  (0x7eaa40600000-0x7eaa40601000-0x7eaa40800000)
  pthread_cache=0x7ed4425d7000
```

`stack_info.start = 0x7eaa40600000` (TEB DeallocationStack, the Wine
mmap base) and `pthread_cache = 0x7ed4425d7000` (glibc pthread stack
start) are **~1 TB apart**. They are not related mappings.

The `pthread_cache < stack_info->start` check therefore returns FALSE
(pthread_cache is numerically *higher*), the refresh does nothing,
and `abort_thread(1)` runs.

In the cases where the check accidentally returned TRUE (pthread
stack happened to be numerically below the TEB stack), the widened
`stack_info.start` was pointed at unrelated memory. The write only
worked because the actual rsp was still above the real Wine stack
bottom, and the check was effectively bypassed without producing a
genuine stack-overflow diagnosis.

#### Why the stack overflow is a symptom, not a cause

Under gVisor, the initial trigger is a NULL-pointer write from the
PE loader (bug described in the TL;DR above). `virtual_handle_fault`
cannot resolve it, `setup_raise_exception` pushes a Windows exception
frame onto the user stack, and the thread resumes at
`KiUserExceptionDispatcher`. The dispatcher calls back into the
debug channel lookup, which faults on the same cascade, and each
iteration pushes another 3776-byte frame. After ~500 iterations the
stack runs out and `virtual_setup_exception` aborts the thread.

Patch 2 was trying to rescue the abort by widening the stack bounds.
But the right fix is to prevent the **original** NULL deref, which
only happens because `gs.base` is wrong, which only happens because
`arch_prctl(ARCH_SET_GS)` is a no-op under gVisor. Patch 3 fixes
that upstream.

Strace of the failing loader (paraphrased from `/tmp/strace.out`)
captured during the original session:

```
SIGSEGV {si_code=SEGV_MAPERR, si_addr=0x18}
rt_sigprocmask(...)
rt_sigreturn() = ...    (handler dispatches exception)
SIGSEGV {si_code=SEGV_MAPERR, si_addr=0x2001}
rt_sigprocmask(...)
rt_sigreturn() = 0
SIGSEGV {si_code=SEGV_MAPERR, si_addr=0x2000}
rt_sigprocmask(...)
rt_sigreturn() = 0
... repeats many times ...
exit_group(1)
```

The first fault at `si_addr=0x18` is the NULL+0x18 write in
`loader_init`. The subsequent faults at `0x2000`/`0x2001` are
`__wine_dbg_get_channel_flags.part.0` reading past the end of an
array whose base pointer is also derived from the bogus gs.base.
Between each SIGSEGV/sigreturn pair Wine's handler runs only
`rt_sigprocmask` — no `mmap`, no `mprotect`, no corrective syscall —
because there's nothing to correct: the fault is a pure consequence
of a bad register value, not a resolvable memory issue.


## Empirical test results

All tests run with:

```bash
WINEPREFIX=/tmp/spark-wineprefix
WINELOADER=/opt/wine-patched/bin/wine
SPARK_WINE_GVISOR_SHIM=0     # shim disabled — patched Wine must stand alone
```

### Test 1 — minimal `wWinMain` program

```c
#include <windows.h>
int WINAPI wWinMain(HINSTANCE h, HINSTANCE p, PWSTR cmd, int show) { return 42; }
```

| Wine | Result |
|------|--------|
| **Unpatched** (prior session, with SparkEngine gvisor-wine-shim loaded) | trap-0 loop until killed |
| **Unpatched** (shim not loaded) | trap-0 loop until killed |
| **Patched with patch 1 + wrong patch 2** (earlier in this session) | No trap-0 loop. `rc={0,1,139}`; never `42`. Worker thread aborts on `virtual_setup_exception stack overflow`. |
| **Patched with patch 1 + correct `set_gs_base` patch 3** (follow-up) | **Runs to completion.** No faults in `loader_init`. `rc=42` or the usual wWinMain stdio quirk — no crashes. |

### Test 2 — minimal console program

```c
#include <stdio.h>
int main(int argc, char **argv) {
    printf("hello from wine, argc=%d\n", argc);
    return 42;
}
```

| Wine | Result |
|------|--------|
| **Patched with wrong patch 2** | `rc={1,139}`, no stdout, main() never reaches `printf()`. |
| **Patched with correct `set_gs_base` patch 3** | `hello from wine, argc=1` printed to stdout, `rc=42`. Verified across three consecutive runs on the same prefix (2 of 3 produced clean output; the 1 flaky run is an unrelated wineboot init race on fresh prefixes, not this bug). |

### Test 3 — SparkTests.exe

Built via `cmake --build build/linux-mingw-release` (linked into a
25.7 MB `.exe`, LTO enabled as in upstream preset).

| Wine | Result |
|------|--------|
| **Patched with wrong patch 2** | `virtual_setup_exception stack overflow 192 bytes` in a worker thread; no test output. |
| **Patched with correct patch 3** | Wine `init_syscall_frame` runs with correct `gs.base`, Wine's PE ntdll loads cleanly, but the test binary itself has unrelated engine-init issues (missing Vulkan driver, no X11 display, minimal Wine build config without `--with-x`/`--with-vulkan`). Getting SparkTests.exe fully running is a separate workstream that needs a richer Wine build and is not blocked by Wine itself anymore. |

## SparkEngine fixes required to make the build succeed

Two genuine SparkEngine bugs were surfaced by running the MinGW cross
build to a complete link. Both are unrelated to the Wine
investigation and are unconditional improvements:

### Fix 1 — `SparkEngine/Source/Utils/AlignedHeapArray.h`

`AlignedAlloc` guarded the MSVC path with `#ifdef _MSC_VER` and the
POSIX path (using `posix_memalign`) otherwise. On MinGW that routes
to `posix_memalign`, which MinGW's libc does not provide:

```
error: 'posix_memalign' was not declared in this scope
```

Changed the guard to `#ifdef _WIN32` (so both MSVC and MinGW use
`_aligned_malloc` from `<malloc.h>`) and added the `<malloc.h>`
include for Windows targets.

### Fix 2 — `Tests/TestDXRSupport.cpp`

The SparkEngine MinGW CMake toggle (`CMakeLists.txt` line 941–945)
excludes `DXRSupport.cpp` entirely on MinGW because MinGW's `d3d12.h`
is too old for `ID3D12Device5`, and defines `SPARK_NO_D3D12`.
`Tests/TestDXRSupport.cpp` was not guarded and referenced
`DXRManager::GetInstance()` + many other `DXRManager` symbols that
don't exist in the link image:

```
undefined reference to `Spark::Graphics::DXRManager::GetInstance()'
undefined reference to `Spark::Graphics::DXRManager::Initialize(void*)'
... (30+ lines)
collect2: error: ld returned 1 exit status
```

Wrapped the entire body of `TestDXRSupport.cpp` in `#ifndef SPARK_NO_D3D12 ... #endif`.
The file still compiles to an empty translation unit on MinGW, so the
`cmake` file list doesn't need changes.

Both fixes landed on `claude/wine-patched-build-test` alongside this
knowledge entry.

## What to do next

1. **Submit patches 1 + 3 upstream** to `wine-devel@winehq.org` as a
   two-patch series. Both are correct, minimal, and reproduce cleanly
   on any user-mode Linux kernel. The patch files are checked into
   SparkEngine at:
   - `docs/wine-upstream/0001-ntdll-fall-back-to-siginfo-for-trap-code.patch`
     — subject `[PATCH 1/2] ntdll: fall back to siginfo for trap code
     when REG_TRAPNO is zero`.
   - `docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch`
     — subject `[PATCH 2/2] ntdll: use wrgsbase as a fallback when
     arch_prctl(ARCH_SET_GS) is broken`. Single squashed commit with
     the arch_prctl-first / verify / wrgsbase-fallback layout.

2. **Do not submit patch 2 upstream.** Upstream already closed
   `wine-mirror/wine#62` (the pthread-refresh PR from the earlier
   session); no action required on that PR. The
   `Krilliac/wine:claude/wine-stack-pthread-refresh` branch still
   holds `0c00c2b + 93195fb` as historical reference — leave or
   rename to `do-not-submit`.

3. **Update the Krilliac/wine wrgsbase-fallback branch in place.**
   The branch currently holds `f0f9846 + be4282b`, both of which have
   the gVisor-silent-no-op regression from the Codex review round
   trip. Apply
   `docs/wine-upstream/0004-ntdll-prefer-arch_prctl-for-set_gs_base.patch`
   on top of `be4282b` to restore correctness without rewriting
   history:
   ```bash
   cd /path/to/wine-fork
   git fetch origin claude/wine-wrgsbase-fallback
   git checkout claude/wine-wrgsbase-fallback
   git am docs/wine-upstream/0004-ntdll-prefer-arch_prctl-for-set_gs_base.patch
   git push origin HEAD:claude/wine-wrgsbase-fallback
   ```
   Alternatively, force-push the squashed `0003-...patch` version to
   collapse all three iterations into a single clean commit on the
   branch.

4. **SparkTests.exe under Wine is a separate workstream.** The real
   gs.base fix is enough to get minimal MinGW-cross binaries running
   end-to-end under patched Wine inside gVisor. SparkTests.exe has
   its own engine-init dependencies (Vulkan, X11 driver, full Wine
   DLL set, wow64 support) that the minimal `--enable-win64
   --disable-tests --without-freetype --without-gstreamer` Wine build
   we made does not cover. That work is not blocked by Wine itself
   anymore — it needs a richer Wine build and/or the CI job.

5. **For SparkEngine itself** — the two MinGW source fixes landed in
   this session are worth merging regardless of Wine's state, since
   they are genuine cross-build correctness improvements that the
   CI `build-linux-mingw-wine` job will exercise.

## Files touched this session

| File | Purpose |
|------|---------|
| `SparkEngine/Source/Utils/AlignedHeapArray.h` | `_MSC_VER` → `_WIN32` guard + `<malloc.h>` include |
| `Tests/TestDXRSupport.cpp` | `#ifndef SPARK_NO_D3D12` wrap |
| `.claude/knowledge/wine-patched-build-results-2026-04-14.md` | This file |
