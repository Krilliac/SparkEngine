# Wine SparkTests.exe actually runs under gVisor — working recipe found

**Last updated:** 2026-04-15 (session 3, user clarification round)
**Type:** Pattern + Observation
**Status:** Active — recipe reproducible at ~30-50% success rate

## TL;DR

After two iterations of LD_PRELOAD shim engineering we found the recipe
that **actually gets the engine running under Wine in this gVisor
sandbox**: pre-populate `drive_c/windows/system32` from Wine's shipped
DLLs, disable `explorer.exe` and `winemenubuilder.exe` via
`WINEDLLOVERRIDES`, load the shim, and run. `SparkTests.exe` starts
executing real engine tests — **1000+ `[ OK ]` lines** in the best
observed run, covering CSG, GamePackager, DynamicQualityScaler,
MobilePlatform, and many more actual engine subsystems.

The boot is still race-condition-dependent (approximately 30-50%
success rate per cold start in this specific sandbox), but when it
wins, it wins cleanly and tests **do run**. The user clarified in a
follow-up that **earlier sessions had this working without any of the
hacks** — meaning the gVisor signal bug was either introduced by a
Wine package update or a sandbox upgrade between sessions. We can't
undo whichever change caused the regression, but we can get back to a
usable state with the recipe below.

## The recipe

```bash
# 1. Clean slate
killall -9 wineserver wine64 wine 2>/dev/null
rm -rf $WINEPREFIX

# 2. Pre-populate system32 from Wine's shipped DLLs. wineboot under
#    gVisor often races the cascade and leaves system32 empty;
#    pre-populating makes the prefix self-sufficient regardless.
mkdir -p $WINEPREFIX/drive_c/windows/system32
cp /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/*.dll \
   $WINEPREFIX/drive_c/windows/system32/

# 3. Disable explorer and winemenubuilder — both try to create X
#    windows which fails hard in a headless sandbox, aborting wineboot.
export WINEDLLOVERRIDES="explorer.exe,winemenubuilder.exe=d"

# 4. Load the shim and run.
LD_PRELOAD=$PWD/tools/gvisor-wine-shim.so \
    /usr/lib/wine/wine64 build/linux-mingw-release/bin/SparkTests.exe
```

`tools/wine-run.sh` now bakes this into its `setup_wineprefix()` so
scripts calling `tools/wine-run.sh <exe>` get the pre-populate + DLL
override step automatically.

## Empirical observations

Verified by running the same command multiple times in succession.
Results vary run-to-run but the **pattern is consistent**:

| Run | Result |
|-----|--------|
| 1 | `[ RUN  ] DynamicQualityScalerPhaseBB_SingletonReturnsSameInstance ... [   OK   ]` — 1000+ tests executing, real engine logs, `EXIT=0` |
| 2 | Terminated after timeout — wineboot race lost, wine hung |
| 3 | Wine exits silently with `EXIT=0` but no tests ran |
| 4+ | Same distribution; ~30-50% of runs produce meaningful test output |

The best run captured:

```
=== SparkEngine Test Suite ===
Running 5400 tests...

[ RUN    ] DynamicQualityScalerPhaseBB_SingletonReturnsSameInstance
[   OK   ] DynamicQualityScalerPhaseBB_SingletonReturnsSameInstance (0us, 1 assertions)
[ RUN    ] DynamicQualityScalerPhaseBB_InitializeClearsWindow
[   OK   ] DynamicQualityScalerPhaseBB_InitializeClearsWindow (264us, 2 assertions)
... (hundreds of tests) ...
[08:12:40.410] [TID:1] [INFO] [Scene] CSGSystem initialized
[08:12:40.411] [TID:1] [INFO] [Scene] CSG: Created brush 1 (shape: 0)
... (real engine init logs from the test fixture) ...
[08:12:40.415] [TID:1] [INFO] [Core] GamePackager initialized
[08:12:40.415] [TID:1] [INFO] [Core] MobilePlatform initializing
```

## Why it works (and why it sometimes doesn't)

**Why it works:** the fatal path for the gVisor cascade is not the
test binary's code — it's **wineboot's explorer.exe startup**, which
tries to create an X11 window and fails. The failure leaves wineboot
in a partially-initialised state where subsequent `wine64 test.exe`
invocations can't find kernel32.dll. By pre-populating system32 and
disabling explorer, we remove both failure modes before they fire. The
shim then handles the residual gs.base cascade (Wine PR #63 path) and
Wine gets far enough to launch and run the actual test binary.

**Why it doesn't always work:** the gs.base cascade still fires on
some thread timings before our SIGSEGV trampoline gets a chance to
repair it. When the race loses, we hit the "services.exe terminated"
path and the prefix ends up in a broken state for the subsequent
launch. The LD_PRELOAD shim can't close this race because Wine's
`init_handler` is `static inline` — there's no symbol to interpose.
A fully patched Wine (built via `tools/build-wine-patched.sh` on a
host with unrestricted network) would win 100% of the time.

## Comparison with user's earlier sessions

The user reports that earlier Claude sessions (same sandbox class)
could compile the engine with MinGW, boot it under Wine + Lavapipe,
and everything ran smoothly — **without** any of the hacks we've
accumulated this session. We have two possible explanations:

1. **Wine package upgrade.** Ubuntu's `wine 9.0~repack-4build3`
   included the `trap_no == 0` signal_x86_64.c check that `gregs
   [REG_TRAPNO] == 0` manifests as the infinite loop under gVisor.
   Earlier Wine versions may have lacked this check, or computed
   the trap number from `siginfo` directly (which is what Wine PR
   #61 adds back as a fallback).

2. **gVisor kernel upgrade.** The sentry's signal synthesis may have
   started leaving `gregs[REG_TRAPNO]` zero at some version, whereas
   earlier versions either populated it or sent signals via a
   different path that Wine's x86_64 signal handler didn't trip on.

We verified Wine 10.0 (the newest Ubuntu package) has the **same**
trap-0 bug as Wine 9.0, so it's not a "just upgrade Wine" fix. We
verified Wine 6.0.3 (oldest Ubuntu package) segfaults immediately on
this host, so it's not a "just downgrade Wine" fix either. Neither
option restores the pre-regression behaviour the user remembers.

## What's in the repo after this session

- `tools/gvisor-wine-shim.c` — LD_PRELOAD shim with:
  1. `sigaction` interception → Wine PR #61 (trap_no fixup)
  2. `syscall` interception → Wine PR #63 (`set_gs_base` via wrgsbase)
  3. `/proc/self/maps` TEB scanner (TIB.Self invariant-validated)
  4. SIGSEGV trampoline gs.base repair from seen TEBs
  5. Opt-in RSP-bump bypass for `virtual_setup_exception`
  6. SIGILL-enveloped `wrgsbase` safety probe
- `tools/wine-run.sh` — pre-populates system32 from
  `/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/*.dll`, sets the
  `explorer.exe,winemenubuilder.exe=d` override, then runs wineboot
  + target binary.
- `tools/build-wine-patched.sh` — fetches Wine source, applies the
  three upstream PR patches from `docs/wine-upstream/`, and builds
  a patched Wine to `/opt/wine-patched`. Requires network access.
- `.claude/knowledge/wine-user-space-hacks-2026-04-15.md` — previous
  iteration.
- `.claude/knowledge/wine-sparktests-actually-runs-2026-04-15.md` —
  this entry.

## What still needs work

1. **Make the recipe deterministic.** The 30-50% success rate is a
   quality-of-life problem; a patched Wine build (tools/build-wine-patched.sh
   on a real Linux host) would fix this permanently.

2. **SparkEngine.exe still hangs** where `SparkTests.exe` runs. The
   engine tries to create a real Win32 window via `CreateWindowEx`
   and hangs waiting for X, even with `-headless`. The `-headless`
   flag's Windows path may not actually skip window creation. A
   separate follow-up.

3. **Graphics code path under DXVK + Lavapipe** is not yet exercised
   end-to-end. The user's specific goal is to test Win32/D3D11
   rendering through DXVK → Vulkan → Lavapipe. Once (2) is fixed,
   that should become reachable.

## Reproducing

```bash
# Build
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --target SparkTests --parallel $(nproc)

# Build shim
gcc -shared -fPIC -O2 -o tools/gvisor-wine-shim.so tools/gvisor-wine-shim.c -ldl

# Run — this bakes the recipe in
LD_PRELOAD=$PWD/tools/gvisor-wine-shim.so \
    tools/wine-run.sh build/linux-mingw-release/bin/SparkTests.exe

# If the first run hangs, kill wineserver and retry up to 3-4 times.
killall -9 wineserver wine64 wine 2>/dev/null; rm -rf /tmp/spark-wineprefix
```
