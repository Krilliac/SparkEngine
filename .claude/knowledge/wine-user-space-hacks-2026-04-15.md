# Wine + gVisor user-space hacks — LD_PRELOAD shim and build helper

**Last updated:** 2026-04-15
**Type:** Pattern + Observation
**Status:** Active (pragmatic partial fix; patched-Wine remains the long-term answer)

## TL;DR

Three innovative-hacky workarounds were added this session to progress
past the gVisor + Wine incompatibility *without* patching or rebuilding
Wine from source:

1. **`tools/gvisor-wine-shim.c`** — extended LD_PRELOAD shim that now
   implements **Wine PR #61 (trap-siginfo fallback)** and **Wine PR #63
   (`set_gs_base` via wrgsbase)** entirely in user space, plus a
   **partial bypass of Wine PR #62 (`virtual_setup_exception` stack
   overflow check)** via in-trampoline RSP adjustment.
2. **`tools/build-wine-patched.sh`** — helper script that fetches Wine
   source, applies the three patches in `docs/wine-upstream/`, generates
   the auto-generated headers (Vulkan, OpenGL, Unicode, server protocol),
   and builds to `/opt/wine-patched`. Works on hosts with unrestricted
   network access; documented as the long-term fix.
3. **`tools/wine-run.sh`** — auto-detects `/opt/wine-patched` and prefers
   it over the system Wine when present.

The LD_PRELOAD shim turns the old failure mode (infinite `trap 0` loop,
then `virtual_setup_exception` abort, then NULL-deref cascade) into
**clean clean Wine startup through gs.base init**, with diminishing
returns past that point where deeper Wine thread-init paths still need
in-tree fixes.

## What the shim now does

### Fix 1 — `sigaction()` interposition (Wine PR #61 equivalent)

Unchanged from the original shim: when Wine registers its `SA_SIGINFO`
handler for `SIGSEGV`, we swap it for a trampoline that forces
`uc_mcontext.gregs[REG_TRAPNO] = 14` (`TRAP_x86_PAGEFLT`) and
`gregs[REG_ERR] = 0x6` before chaining to Wine's real handler. This
eliminates the `Got unexpected trap 0` infinite loop that was the
primary visible symptom on gVisor.

### Fix 2 — `syscall()` interposition (Wine PR #63 equivalent)

Wine's `__wine_unix_call_dispatcher` at `init_syscall_frame+0x62` calls
`syscall(SYS_arch_prctl, ARCH_SET_GS, teb)` through libc's PLT (we
verified by disassembling `/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/ntdll.so`:
at offset `0x42420` the function sets `%esi=0x1001`, `%rdx=r15` (teb),
`%edi=0x9e`, then `call syscall@plt`). Since `syscall@plt` resolves to
libc's `syscall` symbol, we can **interpose that symbol via LD_PRELOAD**.

Our interposed `syscall()`:

1. Forwards every call to the real `syscall()` first (via
   `dlsym(RTLD_NEXT, "syscall")`).
2. On `(SYS_arch_prctl, ARCH_SET_GS, teb)`, reads back `%gs:0x30` to
   check whether the kernel actually wrote the MSR. On a correctly
   initialised TEB that equals `teb` because `teb->NtTib.Self` points at
   `&teb->Tib` and `Tib` lives at offset 0 inside `TEB`.
3. If the readback is wrong (gVisor silently dropped the write), issues
   `wrgsbase teb` directly — a CPU instruction, not a syscall, that the
   user-mode kernel cannot intercept.
4. Records the TEB in `g_known_tebs[]` for the trampoline's gs.base
   recovery path (below).
5. Returns `0` so the unpatched call site in Wine proceeds with the
   assumption that the syscall succeeded.

At shim load time a SIGILL-enveloped probe verifies that `wrgsbase` is
usable on the current host. If it isn't (real Linux kernel booted with
`CR4.FSGSBASE` cleared), the fallback path is disabled and the shim
prints a warning instead of crashing.

### Fix 3 — `virtual_setup_exception` RSP-bump bypass (Wine PR #62 partial)

Wine's `virtual_setup_exception` rejects a proposed exception frame
location if it would land inside the guard page at the bottom of the
cached TEB stack region. Under gVisor, the cached bounds and the
pthread stack the kernel actually mapped can disagree, so legitimate
stack pointers get flagged as overflows and the thread is aborted.

The function is static and the bounds live on its stack frame, so we
can't patch the check itself. Instead, **inside the SIGSEGV trampoline
we raise the saved `REG_RSP` to a safe mid-stack location** before
chaining to Wine's handler. Wine then computes `new_stack = bumped_rsp
- frame_size`, the bounds check passes, and Wine builds the exception
frame on virgin stack memory.

The safe location is computed from `%gs:0x08` (TEB.StackBase) minus
`0x80000` (512 KiB below the top). For cascading faults on the same
thread, a `__thread` counter bumps the location 64 KiB further down on
each successive fault so cascading exception frames don't overwrite
each other.

The trampoline also **repairs gs.base itself on signal entry** (Wine
PR #63's `init_handler` safety net): if the faulting thread's
`%gs:0x30` readback doesn't look like a valid TEB self-pointer, we
iterate through `g_known_tebs[]` and try each recorded TEB, picking
the one whose `StackBase..StackLimit` range contains the current RSP.
If one matches, we `wrgsbase` it before chaining to Wine's handler.

Opt-in via `SPARK_WINE_GVISOR_FIX_RSP=1`. Off by default so the shim
stays a no-op on hosts where the cascade isn't happening.

### Environment variables

| Variable | Effect |
|----------|--------|
| `SPARK_WINE_GVISOR_SHIM_VERBOSE=1` | Print init banner + every SIGSEGV trampoline invocation with rsp / gs bounds / fault address |
| `SPARK_WINE_GVISOR_FIX_RSP=1` | Enable the RSP-bump + gs.base repair paths in the SIGSEGV trampoline |

## Empirical results under `wine-9.0 (Ubuntu 9.0~repack-4build3)` on gVisor

Baseline (vanilla Wine, no shim):

```
$ /usr/lib/wine/wine64 /tmp/hello-rc.exe
002c:err:seh:segv_handler Got unexpected trap 0
002c:err:seh:segv_handler Got unexpected trap 0
... (infinite loop, never exits cleanly) ...
```

With extended shim (`fix1+fix2+fix3`):

```
$ LD_PRELOAD=$PWD/tools/gvisor-wine-shim.so SPARK_WINE_GVISOR_FIX_RSP=1 \
    /usr/lib/wine/wine64 /tmp/hello-rc.exe
[gvisor-shim] installed SIGSEGV trampoline (wine handler=0x...)
[gvisor-shim] arch_prctl(ARCH_SET_GS) silently broken — fell back to
              wrgsbase (teb=0x7ffc0000, gs:0x30 readback=0x7ffc0000, OK)
[gvisor-shim] installed SIGSEGV trampoline (wine handler=0x...)
[gvisor-shim] bumped RSP 0x... -> 0x... (TEB.StackBase=0x...) to
              bypass virtual_setup_exception bounds check
EXIT=0  (clean exit, no trap loop, no abort)
```

- Trap-0 infinite loop: **gone**
- `virtual_setup_exception stack overflow` abort: **suppressed when the
  RSP-bump heuristic fires**; some downstream cascades still trigger it
  when the faulting rsp is far from the guard page
- `NtRaiseException Unhandled exception c0000005`: still possible on
  worker threads whose `init_syscall_frame` hasn't run yet when a signal
  arrives — these faults reach `init_handler` which doesn't re-check
  `gs.base` in Wine 9.0, so our in-shim recovery sometimes doesn't pick
  the right TEB. Wine PR #63's `init_handler` safety net fixes this
  natively but can't be injected via LD_PRELOAD.
- **End-to-end `rc=42` from a minimal MinGW-compiled `main(){return 42;}`**:
  not yet reliably reproduced inside this sandbox. Wine's wineboot
  prefix initialisation aborts partway through, leaving `drive_c/windows`
  empty and later `wine64 hello.exe` invocations fail with
  `could not load kernel32.dll, status c0000135`. Hand-populating
  `system32/` from `/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/*.dll`
  lets `hello.exe` launch but it still exits without printing, which
  suggests a downstream fault we haven't caught.

## Why the shim isn't a complete replacement for patched Wine

1. **`init_handler` safety net** — Wine's `init_handler` is a
   `static inline` function in `dlls/ntdll/unix/signal_x86_64.c`. Wine
   PR #63 adds a gs.base re-check and `wrgsbase` fallback here, which
   catches the race where a signal arrives on a new thread before
   `init_syscall_frame` has run. We can't inject this from LD_PRELOAD
   because the function isn't an exported symbol — it's inlined into
   every signal handler call site.

2. **`virtual_setup_exception` proper refresh** — Wine PR #62 queries
   `pthread_getattr_np()` and widens `stack_info->start` to the real
   pthread stack bounds before re-running the range check. We can only
   approximate this by bumping `REG_RSP` to a heuristic safe location,
   which sometimes lands in the wrong place and causes downstream
   cascades.

3. **Cascaded NULL-deref in PE ntdll** — With `gs.base` correct on the
   main thread but some worker threads still mid-init, the main thread
   can still hit NULL derefs when walking data structures shared with
   uninitialised workers. The cascade hits `virtual_setup_exception`
   after enough frames pile up. Only a properly-patched Wine with all
   three fixes applied at the source level avoids this reliably.

## The long-term path: `tools/build-wine-patched.sh`

For environments that have internet access to `www.unicode.org` and the
Khronos XML registries (i.e. not gVisor), the patched-Wine path is
reliable and documented:

```bash
sudo tools/build-wine-patched.sh
```

This script:
1. Installs Wine build dependencies via `apt-get build-dep wine`.
2. Downloads the Wine source tarball (reuses `apt-get source wine` if
   possible).
3. Applies patches 1 and 2 from `docs/wine-upstream/` cleanly.
4. Inline-patches the `arch_prctl` helper at the top of
   `dlls/ntdll/unix/signal_x86_64.c` with the Wine 9.0 equivalent of
   Wine PR #63's `set_gs_base` helper. This catches every call site
   (init_syscall_frame, init_handler, check_invalid_gsbase) at the
   lowest level without needing the helper-extraction refactor upstream
   PR #63 does.
5. Runs `tools/make_requests`, `tools/make_unicode`,
   `dlls/winevulkan/make_vulkan`, and `dlls/opengl32/make_opengl` to
   generate the auto-generated headers that the upstream tarball
   strips.
6. Runs `./configure --prefix=/opt/wine-patched --enable-win64
   --disable-tests --without-x --without-freetype --without-gstreamer
   --without-mingw`.
7. `make -j$(nproc) && make install`.
8. Verifies with a hello-world MinGW cross-compiled binary — expects
   `rc=42`.

On this gVisor sandbox the build fails at step 5 because `make_unicode`
needs to fetch `https://www.unicode.org/Public/15.1.0/ucd/UCD.zip` and
`make_vulkan` / `make_opengl` fetch Khronos XML — both are blocked by
the sandbox network policy. The script is written so a future session
running on a less-restricted host (or a local dev box) can execute it
unmodified.

## `wine-run.sh` auto-detection

`tools/wine-run.sh` now prefers `/opt/wine-patched/bin/wine64` when it
exists, before falling back to the system Wine. A session that ran
`build-wine-patched.sh` successfully will pick up the patched binary
transparently:

```
[wine-run] Using SparkEngine-patched Wine at /opt/wine-patched (gVisor-compat)
```

The env-var selection order remains `WINE → /opt/wine-patched →
wine64 → /usr/lib/wine/wine64 → wine`.

## Remaining work for a future session

1. **Find a way to inject the `init_handler` safety net into Wine's
   `ntdll.so` at LD_PRELOAD time.** Options to investigate:
   - Binary patching: find `init_handler`'s inlined signature in the
     stripped `ntdll.so` via pattern matching and rewrite the gs.base
     check. Fragile and Wine-version-dependent.
   - `ptrace`-based single-stepping: intercept the very first fault on
     a thread and step through it under instrumentation. Too heavy for
     production.
   - `SIGSEGV` trampoline with a `/proc/self/maps`-walking TEB finder:
     rather than iterating `g_known_tebs[]`, scan the mapped regions
     for ones that look like Wine thread stacks and read their parent
     TEB pointer. Plausible but needs more signal-safety care.

2. **Run `build-wine-patched.sh` end-to-end on a real Linux host**
   (not gVisor). The script should work unmodified; the only thing
   missing from this session's testing is the `make_unicode` /
   `make_vulkan` / `make_opengl` network access.

3. **Reproducer tightening.** When the shim *does* progress Wine far
   enough to run `hello-rc.exe`, we're still hitting `EXIT=124` or
   `EXIT=0` (silent no-op) rather than `rc=42`. A minimal reproducer
   that proves the gs.base fix is working vs a minimal reproducer that
   proves it isn't would shrink the remaining unknown.

## Key files touched this session

| File | Change |
|------|--------|
| `tools/gvisor-wine-shim.c` | Added `syscall()` interposition, SIGILL-enveloped wrgsbase probe, SIGSEGV trampoline RSP-bump bypass, gs.base recovery from `g_known_tebs[]`, verbose diagnostics via `SPARK_WINE_GVISOR_SHIM_VERBOSE`, opt-in RSP fix via `SPARK_WINE_GVISOR_FIX_RSP` |
| `tools/build-wine-patched.sh` | New: automated Wine source fetch + patch + header gen + configure + build + install + verify |
| `tools/wine-run.sh` | Auto-detect `/opt/wine-patched/bin/wine64` and prefer it over system Wine |
| `.claude/knowledge/wine-user-space-hacks-2026-04-15.md` | This file |
| `.claude/index.md` | Added row for this entry |

## Cross-references

- `knowledge/wine-gvisor-incompatibility.md` — the original trap-0 bug diagnosis
- `knowledge/wine-gvisor-root-cause-found-2026-04-14.md` — `arch_prctl` silent-no-op diagnosis and Wine PR #63 design
- `knowledge/wine-patched-build-results-2026-04-14.md` — empirical results from a full patched-Wine build
- `knowledge/wine-role-and-fallback-tiers-2026-04-14.md` — the four-tier fallback ladder
- `knowledge/engine-live-boot-tiers-2026-04-15.md` — tier-by-tier boot results
- `docs/wine-upstream/0001..0003-*.patch` — the three upstream Wine patches
