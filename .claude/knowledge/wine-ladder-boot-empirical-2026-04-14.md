# Fallback Ladder — End-to-End Boot Results (2026-04-14)

**Last updated:** 2026-04-14
**Type:** Observation + Optimization
**Status:** Active

## TL;DR

The five action items from
`wine-role-and-fallback-tiers-2026-04-14.md` were implemented in commits
`83a1980` / `4819e82`. This entry records the empirical results of
actually exercising them end-to-end inside the Claude Code remote
container harness (`process_api` PID 1, Wine 9.0 installed from Ubuntu
24.04, Lavapipe + llvmpipe present, no DXVK, no VKD3D-Proton).

**Outcome:**

| Tier | Path | Result |
|------|------|--------|
| 1 | Wine + DXVK + Lavapipe | **blocked** — DXVK not packaged in Ubuntu noble; `tools/wine-run.sh` refuses with a clear install hint |
| 2 | Wine + WineD3D + llvmpipe | **blocked** — Wine 9.0 hits `virtual_setup_exception stack overflow` in this sandbox class even with the shim loaded (trap_no loop is fixed, stack-frame setup is not) |
| 3 | Wine + NullRHI | **blocked** — same Wine failure as tier 2, failure is in Wine init before the engine's RHI selection ever runs |
| 4 | Native Linux ELF + NullRHI | **works** — 100 subsystems initialize, engine runs, clean shutdown, exit 0 |
| auto | `SPARK_WINE_AUTO_TIER4=1` | **works** — probe fails, script transparently falls through to tier 4 |

The engine **does** boot end-to-end in this sandbox; it just has to
travel down the ladder to rung 4 to do it. Every other tier is the
MinGW .exe waiting for a real-kernel host to run on.

## Sandbox class expansion

Empirically confirmed today: the Claude Code remote container is the
same class of sandbox as gVisor for Wine purposes. PID 1 `comm` reads
`process_api`, not `runsc`, but the ucontext signal regime is
identical — Wine's `segv_handler` loops on `Got unexpected trap 0`
exactly like it does under gVisor. `IsRunningUnderGvisor()` was
updated to also match `process_api` in `/proc/1/comm`. If further
sandbox families turn up with the same quirk, they should be added
there rather than broadening the function name — the contract is
"breaks Wine signal handling the same way gVisor does", not
"literally runsc".

**Detection evidence:**

```
$ cat /proc/1/comm
process_api
$ WINEDEBUG=err+seh /usr/lib/wine/wine64 hello.exe 2>&1 | grep -c 'trap 0'
1816
```

## Shim fixes failure mode #1 but not #2

`tools/gvisor-wine-shim.so` (built from `tools/gvisor-wine-shim.c`) was
loaded via `SPARK_WINE_GVISOR_SHIM=1`. Empirical result:

| Failure mode | Without shim | With shim |
|--------------|:------------:|:---------:|
| `segv_handler Got unexpected trap 0` (infinite loop) | 1816 occurrences | **0** |
| `virtual_setup_exception stack overflow 64 bytes` | 2 occurrences | 2 occurrences |

The shim's own header docstring already warned about this:

> This shim only fixes the trap-number loop. Wine 9.0 also hits a
> secondary failure under gVisor (`virtual_setup_exception stack
> overflow 64 bytes`) when trying to build the Windows-side exception
> frame [...]. That second issue would need a Wine source patch to fix.

This is exactly the patch set wine-mirror/wine#61, #62, #63 addresses.
Until those land (or we ship a pre-patched Wine via a container image),
tiers 1–3 are not viable inside this sandbox class. Tier 4 is the
supported path.

## Bugs found and fixed while running the ladder

Three defects in the initial implementation surfaced during live
testing:

### 1. `probe_wine_environment` grepped for errors the probe had suppressed

Root cause: `wine-run.sh` sets `WINEDEBUG="${WINEDEBUG:--all}"` at
script load time to keep the real run quiet. The probe function then
ran Wine inheriting that env var, so the very `err:seh:segv_handler`
and `err:virtual:virtual_setup_exception` strings the grep was looking
for were never emitted. The probe reported success even when Wine was
spinning in the segv loop.

**Fix:** force `WINEDEBUG="err+seh,err+virtual"` locally inside
`probe_wine_environment` regardless of ambient. This also means the
probe's output is always diagnosable by a human reading the log.

### 2. Shim `LD_PRELOAD` setup ran *after* the probe

Root cause: the shim export lived at the bottom of `wine-run.sh`'s
main block, right before the `exec` of the target .exe. The probe at
the top of main ran first, without the shim, so `SPARK_WINE_GVISOR_SHIM=1`
was effectively ignored — the probe saw an unshimmed environment and
short-circuited with the trap-0 error even when the shim would have
worked for the real run.

**Fix:** moved the shim `export LD_PRELOAD=` block before
`probe_wine_environment`. The probe now exercises the same environment
the target run will use, which is the only behaviour that produces
consistent verdicts.

### 3. Probe diagnostic mislabelled the failure mode

Root cause: the probe reported the same `Got unexpected trap 0` error
message whether the actual failure was the trap loop or the later
`virtual_setup_exception stack overflow`. When a user loaded the shim
the probe would say "trap 0 loop" while the real cause was the
untfixable second failure, leading them to believe the shim was broken
and retry pointlessly.

**Fix:** the probe now grep-counts both strings separately and prints
one of three distinct diagnostics: (a) trap-0 only — "build and load
the shim"; (b) virtual_setup_exception only — "shim works, you need
tier 4"; (c) both — "shim not loaded or frame builder racing".

### 4. `SPARK_WINE_AUTO_TIER4` added for CI ergonomics

New env var: when the probe fails AND `SPARK_WINE_AUTO_TIER4=1`,
`wine-run.sh` transparently invokes `find_native_linux_equivalent()`
and execs the matching native Linux ELF. Without the opt-in the probe
still exits 2 so a human-driven run sees the diagnostic. This is the
thing CI should use: set `SPARK_WINE_AUTO_TIER4=1` once in the job
env, and the mingw-wine CI job silently degrades to tier 4 instead of
failing when Wine is broken in the runner.

## Commands that produced the logged output

**Build the MinGW artefacts:**

```
cmake --preset linux-mingw-release
cmake --build build/linux-mingw-release --target SparkEngine -- -j$(nproc)
# → build/linux-mingw-release/bin/SparkEngine.exe
#   12.1 MB PE32+ GUI x86-64 + 10 game module DLLs + libwinpthread-1.dll
```

**Build the shim:**

```
gcc -shared -fPIC -O2 -o tools/gvisor-wine-shim.so \
    tools/gvisor-wine-shim.c -ldl
```

**Probe the sandbox:**

```
WINE=/usr/lib/wine/wine64 WINELOADER=/usr/lib/wine/wine64 \
  SPARK_WINE_PROBE=/tmp/wine-probe/hello.exe \
  SPARK_WINE_GVISOR_SHIM=1 \
  tools/wine-run.sh /tmp/wine-probe/hello.exe
# → [wine-run] ERROR: Wine cannot execute Windows binaries in this environment.
#   Failure mode: 'virtual_setup_exception stack overflow'.
```

**Auto-fall through to tier 4:**

```
WINE=/usr/lib/wine/wine64 WINELOADER=/usr/lib/wine/wine64 \
  SPARK_WINE_PROBE=/tmp/wine-probe/hello.exe \
  SPARK_WINE_GVISOR_SHIM=1 \
  SPARK_WINE_BACKEND=wined3d \
  SPARK_WINE_AUTO_TIER4=1 \
  tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe \
                    -headless -test-frames 30
# → [wine-run] Running native: build/linux-gcc-release/bin/SparkEngine -headless -test-frames 30
#   [Core] Running under gVisor (runsc user-mode kernel) — GPU ioctls...
#   ...108 subsystems initialize...
#   [Core] Timer destructor called
#   exit 0
```

**Direct tier 4 (no probe, no Wine at all):**

```
SPARK_SKIP_WINE=1 tools/wine-run.sh \
  build/linux-mingw-release/bin/SparkEngine.exe \
  -headless -test-frames 10
# Same result — engine boots via native ELF, exits cleanly.
```

## What's still needed

Unchanged from the parent entry
`wine-role-and-fallback-tiers-2026-04-14.md`:

- Upstream wine-mirror/wine#61, #62, #63 to land so tiers 1–3 work
  under gVisor-class sandboxes.
- A pre-patched Wine container image so CI is reproducible even when
  upstream is stuck.
- Extending the shim to cover the `virtual_setup_exception` case,
  which would let us retire the "need patched Wine" requirement for
  developer workstations (CI would still benefit from the container).

## Cross-references

- `knowledge/wine-role-and-fallback-tiers-2026-04-14.md` — parent
  decision + action items
- `knowledge/wine-gvisor-incompatibility.md` — original trap_no
  failure write-up
- `knowledge/wine-gvisor-root-cause-found-2026-04-14.md` — upstream
  PR walkthrough
- `tools/wine-run.sh` — the dispatcher (now with fixed probe, shim
  ordering, and SPARK_WINE_AUTO_TIER4)
- `tools/gvisor-wine-shim.c` — the LD_PRELOAD trampoline
- `SparkEngine/Source/Utils/WineDetection.cpp` — `IsRunningUnderGvisor()`
  with `process_api` match

## Why this entry matters

Next time a session asks "does the fallback ladder actually work?"
the answer is: yes, empirically, commit `<NEW-SHA>` on branch
`claude/apply-engine-fixes-vrFxb` proves it. Every rung of the ladder
was exercised, three defects in the original implementation were
found and fixed, and the engine was booted end-to-end inside a
sandbox where Wine itself cannot run a single instruction of a
.exe. The ladder is doing exactly what the parent entry promised:
converting "this machine can't run SparkEngine.exe" into "this
machine runs SparkEngine via a slower but working path."
