# Wine + gVisor Incompatibility (and the fixes that matter on real Linux)

**Last updated:** 2026-04-14
**Type:** Observation + Pattern
**Status:** Active

## Problem

Running a MinGW-compiled SparkEngine binary under Wine *inside the sandbox
this session is running in* (gVisor `runsc`, kernel 4.4.0 reported by
`uname -r`) produces an infinite signal loop during Wine's early init:

```
002c:err:seh:segv_handler Got unexpected trap 0
002c:err:seh:segv_handler Got unexpected trap 0
...  (thousands of lines per second, forever)
```

This is **not** a SparkEngine bug. Even a minimal C hello-world
cross-compiled with `x86_64-w64-mingw32-gcc` and executed via
`wine64 hello.exe` reproduces the loop.

## Root cause

Wine's `segv_handler` (`dlls/ntdll/unix/signal_x86_64.c`) reads the CPU
trap number directly from the `ucontext` passed to its signal handler:

```c
#define TRAP_sig(context) ((context)->uc_mcontext.gregs[REG_TRAPNO])

static void segv_handler(int signal, siginfo_t *siginfo, void *sigcontext)
{
    ucontext_t *ucontext = sigcontext;
    switch (TRAP_sig(ucontext)) {
        case TRAP_x86_PAGEFLT:  /* 14 */
            /* handle page fault properly — map the missing page, resume */
            break;
        ...
        default:
            ERR("Got unexpected trap %llu\n", (ULONG64)TRAP_sig(ucontext));
            return;  /* <-- returns WITHOUT resolving the fault */
    }
}
```

On real Linux kernels, when a SIGSEGV is delivered, the kernel populates
`gregs[REG_TRAPNO]` with the x86 trap number (14 for page faults). Under
gVisor's user-mode `runsc` kernel, signals are synthesized without
populating that field, so Wine reads `trap_no == 0`, takes the `default`
branch, logs the "unexpected trap" message, and returns. The faulting
instruction is *not* repaired, so it re-executes and faults again.

## strace evidence

```
48680 rt_sigaction(SIGSEGV, NULL, {sa_handler=SIG_DFL, sa_mask=[], sa_flags=0}, 8) = 0
48683 rt_sigaction(SIGSEGV, {sa_handler=0x7ea220f902e0, ...}, NULL, 8) = 0
48683 --- SIGSEGV {si_signo=SIGSEGV, si_code=SEGV_MAPERR, si_addr=0x70} ---
48683 rt_sigreturn({mask=[]}) = 154
48683 --- SIGSEGV {si_signo=SIGSEGV, si_code=SEGV_MAPERR, si_addr=0x2001} ---
48683 rt_sigreturn({mask=[]}) = 0
48683 --- SIGSEGV {si_signo=SIGSEGV, si_code=SEGV_MAPERR, si_addr=0x2001} ---
...  (repeats forever, no intervening mmap syscalls)
```

Note the absence of `mmap` / `mprotect` between the faults — Wine's
handler returns immediately without repairing the fault.

## Workarounds that do *not* work

- `WINEPRELOADRESERVE=`, `WINEPRELOADRESERVE=00000000-00000000`
- Setting `/proc/sys/vm/mmap_min_addr` to 0 (read-only under gVisor)
- `WINEARCH=win64` with a fresh prefix
- `WINELOADERNOEXEC=1`
- Larger `ulimit -s`
- Running `wineboot --init` first
- Unsharing user/mount/pid namespaces

## Workaround that *does* work (partially)

`tools/gvisor-wine-shim.c` is an LD_PRELOAD shim that intercepts
`sigaction(SIGSEGV, ...)`, captures Wine's real handler, and installs a
trampoline that forces `REG_TRAPNO = 14` and `REG_ERR = 0x6` before
calling Wine's handler. With this shim, Wine's `segv_handler` takes the
expected page-fault path and the trap loop goes away.

```bash
gcc -shared -fPIC -O2 -o tools/gvisor-wine-shim.so tools/gvisor-wine-shim.c -ldl
LD_PRELOAD=$PWD/tools/gvisor-wine-shim.so \
    WINELOADER=/usr/lib/wine/wine64 \
    wine64 build/linux-mingw-release/bin/SparkEngine.exe
```

Or via `tools/wine-run.sh`:

```bash
SPARK_WINE_GVISOR_SHIM=1 tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe
```

**Important limitation:** fixing the trap loop exposes a *second* Wine
bug under gVisor:

```
err:virtual:virtual_setup_exception stack overflow 64 bytes
    addr 0x6fffffcad75e
    stack 0x7ec067000fc0 (0x7ec067000000-0x7ec067001000-0x7ec067200000)
```

`virtual_setup_exception` tries to push a 64-byte Windows exception
frame onto the Windows-side thread stack, but gVisor's mmap placement
doesn't match the `thread_data->low_limit` Wine cached at thread
creation time. The check fails and Wine reports a spurious "stack
overflow", bailing out of whatever operation triggered the fault. The
process then exits with `EXIT=0` but without running our `main()`.

A Wine source patch to `virtual_setup_exception` would be needed to
reconcile the Windows-thread stack bookkeeping with gVisor's allocator.
That is outside the scope of engine-side fixes.

## What works on real Linux

On a conventional Linux kernel (bare metal, a normal VM, a Docker
container running under `runc`, GitHub Actions `ubuntu-24.04` runners,
etc.), neither of the gVisor issues occurs:

- `REG_TRAPNO` is populated correctly, so Wine's segv_handler takes the
  normal page-fault path.
- Thread stacks are mmap'd at addresses matching Wine's expectations,
  so `virtual_setup_exception` sees a valid stack.

The CI job `build-linux-mingw-wine` runs on `ubuntu-24.04` (a real
Linux kernel) and therefore works. Local development can also use Wine
on a real Linux host without needing the shim.

## Engine-side helpers landed in this session

- `SparkEngine/Source/Utils/WineDetection.h|.cpp` — exposes
  `Spark::IsRunningUnderWine()`, `Spark::GetWineVersion()`, and
  `Spark::LogWineEnvironmentIfApplicable()`. Queries
  `ntdll.dll::wine_get_version` at runtime (Wine's public detection
  hook). No-op on non-Windows builds. Called from both `wWinMain`
  (Windows entry point) and Linux `main()` so a cross-host banner shows
  `Running under Wine 9.0` when applicable.

- `tools/gvisor-wine-shim.c` — the LD_PRELOAD trampoline described
  above. Builds via `gcc -shared -fPIC -O2 -o tools/gvisor-wine-shim.so
  tools/gvisor-wine-shim.c -ldl`. Only useful under gVisor-class
  environments; harmless on real Linux.

- `tools/wine-run.sh` — rewritten dispatcher for Ubuntu 24.04
  packaging quirks. The Ubuntu `wine` wrapper always prefers
  `/usr/lib/wine/wine` (32-bit) when that file is `test -x`, even for
  pure 64-bit .exes, which fails with "Exec format error" on any
  system where the 32-bit wine loader can't run (minimal containers,
  gVisor). `wine-run.sh` now resolves to `wine64` / `/usr/lib/wine/wine64`
  directly, bypassing the wrapper. New env var `SPARK_WINE_PROBE=/path/to/probe.exe`
  runs a pre-flight check with a known-good PE binary and bails out
  with a readable error when the environment can't execute Wine at
  all, instead of spinning in the trap 0 loop. New env var
  `SPARK_WINE_GVISOR_SHIM=1` auto-enables `LD_PRELOAD` for
  `tools/gvisor-wine-shim.so`.

- `cmake/toolchains/mingw-w64-x86_64.cmake` + `cmake/mingw-shims/` —
  previously landed (see commit `d2b49bd`). Fetches Microsoft's real
  DirectXMath at configure time so the ~180 `#include <DirectXMath.h>`
  statements in engine code compile against MinGW-w64, and guards the
  ELF `-Wl,-z,relro,-z,now` flag from MinGW's PE linker.

## Quick reference

```bash
# 1. Cross-compile (Linux → Windows)
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --parallel $(nproc)

# 2. Smoke-test under Wine on a real Linux host (CI or dev box)
tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe -test-frames 60

# 3. Smoke-test under Wine inside a gVisor sandbox (bail fast)
SPARK_WINE_PROBE=/tmp/hello.exe \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe
# -> prints "Wine cannot execute Windows binaries in this environment"

# 4. (experimental) Engine under gVisor with the shim — gets past trap 0
#    but still fails on the Wine virtual_setup_exception bug.
SPARK_WINE_GVISOR_SHIM=1 \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe -test-frames 3
```

## References

- Wine source: `dlls/ntdll/unix/signal_x86_64.c` (segv_handler,
  virtual_setup_exception)
- `tools/gvisor-wine-shim.c` (our LD_PRELOAD fix for #1)
- gVisor signal emulation: https://github.com/google/gvisor/tree/master/pkg/sentry/platform
- Ubuntu wine packaging: `/usr/bin/wine-stable` script selects
  `/usr/lib/wine/wine` (32-bit) based on `test -x`, not executability.
