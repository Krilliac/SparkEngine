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

## Patch 2 verdict: DOES NOT WORK — based on a wrong mental model

Patch 2 sometimes prints its intended `warn:virtual:virtual_setup_exception
stack_info start widened from TEB value to pthread value` message and
proceeds — but examination of the cases where it "works" shows the
refresh is widening `stack_info->start` to an address many GB below the
current stack mapping, not into the actual pthread stack. The refresh
happens to unblock the range check by coincidence: the address it
widens to is inside unrelated memory, and the subsequent write only
succeeds because the real rsp is still well above the Wine-allocated
Windows stack's true limit.

### Why the mental model is wrong

Patch 2 (both my original and the cached rewrite) assumes the **TEB
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

### What the real bug #2 is

Under gVisor, Wine's Windows thread stack ends up with rsp very close
to `StackLimit` on the first exception — far lower than
`context.Rsp = StackBase - 0x28` (the value `signal_start_thread()`
initializes it to on line 2900 of `dlls/ntdll/unix/signal_x86_64.c`).

Either:

  1. The thread is running legitimate code deep in Wine's loader
     init (ntdll → kernel32 → ucrtbase → ...) and has consumed
     ~2 MB of stack before reaching the guest main(). Faults during
     that init then don't have room to push an exception frame.
  2. gVisor's signal emulation is synthesising spurious SIGSEGVs at
     non-mapped addresses (`si_addr=0x2000` observed via strace)
     that Wine's `virtual_handle_fault()` cannot resolve, and each
     attempt to raise those as guest exceptions consumes stack.
  3. Wine's thread-entry rsp is being corrupted or reset on gVisor
     between the `context.Rsp = StackBase - 0x28` assignment and
     the first guest instruction.

Strace of the failing loader shows this pattern (paraphrased from
`/tmp/strace.out`):

```
SIGSEGV {si_code=SEGV_MAPERR, si_addr=0x2000}
rt_sigprocmask(...)
rt_sigreturn() = 0
SIGSEGV {si_code=SEGV_MAPERR, si_addr=0x2000}
rt_sigprocmask(...)
rt_sigreturn() = 0
... repeats many times, no intervening mmap/mprotect ...
exit_group(1)
```

Between each SIGSEGV/sigreturn pair, Wine's handler is running and
returning without making any corrective syscall. That means
`virtual_handle_fault(0x2000, ...)` returns `STATUS_ACCESS_VIOLATION`
(0x2000 is not in any Wine-tracked view), and `setup_raise_exception`
runs, but either `virtual_setup_exception` aborts the thread or the
exception dispatcher recurses on the same fault. This is not the
class of bug patch 2 models.

**Implication:** patch 2 (and its cache rewrite) should not be
submitted upstream. The correct upstream fix for bug 2 requires a
different diagnosis — probably instrumenting `signal_start_thread()`
and the loader-init path to capture the real rsp trajectory, or
working with gVisor maintainers on the spurious SIGSEGV delivery.

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
| **Patched, no shim** | No trap-0 loop. `rc={0,1,139}` (non-deterministic); never `42`. Main thread never reaches `return 42`. Worker thread aborts on `virtual_setup_exception stack overflow` before main() runs. |

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
| **Patched, no shim** | `rc={1,139}`, no stdout, no file output from a version that wrote to disk. main() never reaches `printf()`. |

### Test 3 — SparkTests.exe

Built via `cmake --build build/linux-mingw-release` (linked into a
25.7 MB `.exe`, LTO enabled as in upstream preset). Running
`SparkTests.exe --list` under patched Wine produced:

```
preloader: Warning: failed to reserve range 00007ffffe000000-00007fffffff0000
0024:err:virtual:virtual_setup_exception stack overflow 192 bytes
     addr 0x7ea5422ac8a0
     stack 0x7ea541800f40
     (0x7ea541800000-0x7ea541801000-0x7ea541a00000)
     pthread_cache=0x7efb0cc57000
rc=0
```

`rc=0` under piped stdio is the known Wine `wWinMain` exit-code-quirk,
not actual success. The aborted thread is a worker, and no test
output appears — the test runner did not reach its first `TEST()`.

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

1. **Submit patch 1 upstream** to `wine-devel@winehq.org` as a
   standalone fix. It is correct, minimal, and reproduces cleanly on
   any user-mode Linux kernel. Subject line: `[PATCH] ntdll: fall
   back to siginfo for trap code when REG_TRAPNO is zero`.

2. **Abandon patch 2 and its cache rewrite on Krilliac/wine.** The
   diagnosis is wrong. Either rebase the fork off just the trap-code
   fix, or leave the branches as historical reference and mark them
   as "do not submit."

3. **Re-diagnose the real bug 2** in a follow-up session:
   - Instrument `signal_start_thread` to log thread-entry rsp.
   - Instrument `virtual_setup_exception` to log both original_rsp
     (before the `-= size`) and `stack_info.limit`.
   - Run a minimal `int main() { return 0; }` under patched Wine
     inside gVisor and capture which function's call chain is
     consuming the 2 MB of Windows stack between thread entry and
     the first fault.
   - If the root cause is spurious gVisor SIGSEGV at `si_addr=0x2000`,
     file an issue with gVisor rather than trying to patch Wine.

4. **For SparkEngine itself** — the two MinGW source fixes landed in
   this session are worth merging regardless of Wine's state, since
   they are genuine cross-build correctness improvements that the
   CI `build-linux-mingw-wine` job will exercise.

## Files touched this session

| File | Purpose |
|------|---------|
| `SparkEngine/Source/Utils/AlignedHeapArray.h` | `_MSC_VER` → `_WIN32` guard + `<malloc.h>` include |
| `Tests/TestDXRSupport.cpp` | `#ifndef SPARK_NO_D3D12` wrap |
| `.claude/knowledge/wine-patched-build-results-2026-04-14.md` | This file |
