# Wine + gVisor: root cause found and fixed

**Date:** 2026-04-14 (updated later the same day to address Codex review)
**Type:** Observation + Fix
**Status:** **RESOLVED (iteration 3 — final)**
**Session:** deep investigation + empirical fix + Codex review round-trip + verified working

## Iteration history

| Rev | Commit | Strategy | Outcome |
|-----|--------|----------|---------|
| 1 | `f0f9846` | wrgsbase first, arch_prctl fallback, unconditional | Fixes gVisor but flagged by Codex review for SIGILL risk on hosts with `CR4.FSGSBASE` cleared |
| 2 | `be4282b` | Guard wrgsbase on `PF_RDWRFSGSBASE_AVAILABLE`, arch_prctl fallback | Addresses Codex, but **silently no-ops on gVisor** because gVisor under-reports `AT_HWCAP2` and Wine's feature flag is AND-gated on that bit |
| 3 | `6db9694` (this version) | **arch_prctl first, verify via `%gs:0x30`, wrgsbase fallback only on failure** | Fixes gVisor AND avoids unconditional SIGILL on bare-metal hosts. Final. |

Key insight for iteration 3: on bare-metal Linux `arch_prctl`
succeeds, verification passes, and the `wrgsbase` fallback is
**never executed** — so hosts with `CR4.FSGSBASE` disabled simply
never reach the problematic instruction. The only hosts that get
to `wrgsbase` are those where `arch_prctl` has already been proven
broken at runtime, which in practice means user-mode Linux kernels
that do expose the fsgsbase feature to user mode (such as gVisor).
This gets correctness and safety without the Codex trade-off.

## Executive summary

The Wine-under-gVisor failure mode described in the original
`wine-gvisor-incompatibility.md` knowledge entry was **not** a stack
overflow in `virtual_setup_exception` and was **not** fixable by
refreshing pthread stack bounds. The real root cause is much simpler:

**Under gVisor's runsc sentry, the `arch_prctl(ARCH_SET_GS, teb)`
syscall that Wine uses in `init_syscall_frame()` to point
`gs.base` at the Windows TEB is silently ignored — the syscall
returns success but gs.base is never written.** Every subsequent
`%gs:0x30` read in Wine's PE ntdll (which is how
`NtCurrentTeb()` is implemented on x86_64 Linux) returns garbage,
and the first function that dereferences that garbage as a TEB
pointer (`loader_init` in `dlls/ntdll/loader.c`) faults on a
NULL-derived pointer write.

Fix: use the `wrgsbase` instruction (which is a direct CPU
instruction, not a syscall) to write gs.base, and fall back to
`arch_prctl` only if wrgsbase has no effect. Patch against Wine
9.x master lives at
[`docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch`](../../docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch).

## Verification

Built `wine-11.6` locally from `Krilliac/wine` with the fix applied
and ran:

```bash
cat > hello.c <<'EOF'
#include <stdio.h>
int main(void) { puts("hello from wine"); return 42; }
EOF
x86_64-w64-mingw32-gcc hello.c -o hello.exe
WINEPREFIX=/tmp/clean-prefix /opt/wine-patched/bin/wine hello.exe; echo rc=$?
```

**Output after fix:**

```
hello from wine, argc=1
rc=42
```

**Output before fix** (same binary, same environment, unpatched Wine):

```
(hangs in an infinite SIGSEGV loop at si_addr=0x2000, eventually
 killed by virtual_setup_exception stack overflow or wine loader
 SIGSEGV, exit code 1/139)
```

## How the diagnosis was made

The session followed a long diagnostic path through increasingly
targeted instrumentation of the Wine signal handler and related
init code. Key breadcrumbs in order:

1. **Strace revealed a cascade, not an infinite single fault.**
   The faulting addresses walked: `0x18 → 0x2001 → 0x2000 → 0x2000 ...`.
   The rsp decreased by exactly 3776 bytes between each iteration —
   that's `sizeof(struct exc_stack_layout)` plus alignment, meaning
   each iteration was pushing a new Windows exception frame.

2. **Instruction bytes at the faulting rip disassembled to
   `mov %rax,0x18(%rdi)`** which, with `rdi=NULL`, produces a
   `si_addr=0x18` fault. Looking up the offset in `ntdll.dll.so`
   pinned it to `loader_init+0x35b` — the line `peb->LdrData = &ldr;`
   in `dlls/ntdll/loader.c:4443`.

3. **`rdi` came from `mov 0x60(%r12),%rdi`** at `loader_init+0x320`,
   which is `peb = NtCurrentTeb()->Peb`. That means `rdi == 0` only
   when `teb->Peb == 0`, or when `r12` is not a real TEB.

4. **`r12` came from `mov %gs:0x30,%r12`** at the top of
   `loader_init`, which is `NtCurrentTeb()`. That means `r12` was
   whatever `%gs:[0x30]` loaded.

5. **The diagnostic print showed `r12 = 0x7ff734910000`** — an
   address in a glibc-private memory region, not the real TEB.
   The real TEB was `0x7ffc0000` as set by `init_teb()`.

6. **`rdgsbase` and `arch_prctl(ARCH_GET_GS)` both returned 0** for
   the current gs.base, but a direct `mov %gs:0x30, rax` returned
   `0x7ff734910000`. Interpretation: gVisor's readback paths are
   broken, but the effective gs.base is non-zero.

7. **Writing sentinel via `arch_prctl(ARCH_SET_GS, 0xdead000000000000)`
   returned -1** (not success, not ignored — actually failed). So
   the syscall *doesn't* even succeed under gVisor.

8. **Writing via `wrgsbase` and reading back via `%gs:0x30` worked
   correctly** (see `/tmp/gs-test2.c`). That's the fix vector:
   replace `arch_prctl(ARCH_SET_GS)` with `wrgsbase`.

## The fix architecture

Three call sites in `dlls/ntdll/unix/signal_x86_64.c` need to write
gs.base:

| Function | When it runs | Critical? |
|---|---|---|
| `init_syscall_frame` | Once per thread start, before user code | Yes — this is the one that matters |
| `check_invalid_gsbase` | Signal handler copy-protection workaround | Needed for consistency |
| `init_handler` | Entry of every signal handler | Safety net for late fixups |

All three were doing `arch_prctl(ARCH_SET_GS, teb)`. The patch
introduces a `set_gs_base(teb)` helper that:

1. Issues `wrgsbase teb` as the primary path (direct instruction,
   cannot be intercepted by an emulator that still reports FSGSBASE
   as a user-visible feature).
2. Verifies via `mov %gs:0x30, probe`. On a correctly-initialised
   TEB, `%gs:0x30` = `teb->NtTib.Self` = `teb` (the TIB Self pointer
   equals the TEB base since TIB is at offset 0 in TEB).
3. Falls back to `arch_prctl(ARCH_SET_GS, teb)` only if the read-back
   did not match — so the helper stays safe on hosts where
   `CR4.FSGSBASE` is cleared and `wrgsbase` traps or is ignored.

On bare-metal Linux the primary path works and the fallback is a
no-op. On gVisor the primary path works and the fallback is never
reached. The only environment where the fallback matters is a host
that both (a) disables FSGSBASE in CR4 and (b) implements arch_prctl
correctly — a combination that's becoming rare but still exists on
some older kernels / specific sandbox configurations.

The new `init_handler` path (late fixup) is important because the
very first fault on a newly-created thread can happen before
`init_syscall_frame` gets a chance to run. In that narrow window,
the thread's gs.base is still wrong, `loader_init` has already
tripped on a NULL deref, and the signal handler is the first chance
to repair gs.base for the subsequent resume/retry path.

## How patches 1-3 relate — all three are upstream as open PRs

All three fixes in `docs/wine-upstream/` have been submitted to
`wine-mirror/wine` as separate open pull requests. Current status:

| Patch | Upstream PR | State | Notes |
|---|---|---|---|
| 0001: trap-siginfo fallback | [`wine-mirror/wine#61`](https://github.com/wine-mirror/wine/pull/61) | **Open** | Unblocks the `Got unexpected trap 0` loop on hosts where `gregs[REG_TRAPNO] == 0`. Independent of the other two. |
| 0002: pthread stack refresh | [`wine-mirror/wine#62`](https://github.com/wine-mirror/wine/pull/62) | **Open** (closed briefly, reopened same day) | Fixes `virtual_setup_exception` killing threads with a bogus "stack overflow N bytes" on sandboxes where the pthread stack mapping is narrower than the TEB's cached `DeallocationStack`/`StackBase`. Uses a signal-safe per-thread cache populated at thread init. **Does not** fix the gs.base cascade this session investigated — that's a different bug fixed by #63. |
| 0003: wrgsbase / arch_prctl-first | [`wine-mirror/wine#63`](https://github.com/wine-mirror/wine/pull/63) | **Open** | The real fix for the `arch_prctl(ARCH_SET_GS)` silent no-op on gVisor. Contains three commits showing the Codex review evolution: `f0f9846` (wrgsbase first), `be4282b` (PF_RDWRFSGSBASE guard), `5cc6634` (arch_prctl-first final). |

**Important correction to an earlier draft of this file:** a previous
version said patch 2 was "wrong mental model, withdraw" and should
be dropped in favour of patch 3. That was an overstep. Patches 2 and
3 fix **distinct failure modes** of the same
"Wine-on-user-mode-Linux-kernel" incompatibility class, and Krilliac
(the author) upstreamed both. Patch 2's TEB-vs-pthread stack-bounds
refresh is a no-op on hosts where the two agree (including my test
reproducer), but it's a real fix on sandbox runtimes whose pthread
allocator hands Wine a narrower mapping than the TEB recorded at
thread creation. My reproducer hit patch 3's case, not patch 2's —
but that doesn't mean patch 2 is wrong, just that my specific
binary + gVisor version didn't exercise it.

## What's in this branch for the Wine fork

If you want to rebuild a patched Wine locally that contains all
three fixes, apply them in order:

```bash
cd /path/to/wine-fork
git checkout -b fix/gvisor-compat wine-9.0
git am /path/to/SparkEngine/docs/wine-upstream/0001-ntdll-fall-back-to-siginfo-for-trap-code.patch
git am /path/to/SparkEngine/docs/wine-upstream/0002-ntdll-refresh-stack-info-from-pthread-under-gVisor.patch
git am /path/to/SparkEngine/docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch
```

Build from that branch and test. The verification test is:

```bash
echo '#include <stdio.h>
int main(void) { puts("hello"); return 42; }' > /tmp/hello.c
x86_64-w64-mingw32-gcc /tmp/hello.c -o /tmp/hello.exe
/path/to/patched/wine /tmp/hello.exe; echo rc=$?
# Expected: hello\nrc=42
```

For the minimum set that fixes the specific gs.base cascade this
session reproduced, patches 1 + 3 alone are sufficient. Patch 2 is
additionally needed on sandboxes where the TEB/pthread stack bounds
disagree; on bare-metal Linux and on my test gVisor instance it's a
signal-safe no-op.

## Open items

1. **Watch the three upstream PRs.** Respond to reviewer feedback
   promptly. Each PR is small and localised, so changes should be
   fixups rather than restructures.

2. **SparkTests.exe under patched Wine.** Minimal hello-world programs
   now run to completion, but `SparkTests.exe` has its own engine-init
   issues that are orthogonal to the gs.base fix — missing Vulkan,
   missing X11, minimal Wine build configuration. Getting the full
   engine test suite running under this Wine is a separate workstream
   that needs a richer Wine build (`--with-x`, `--with-vulkan`, full
   DLL set, wow64 support) and is not blocked by Wine itself anymore.

3. **Do not "withdraw" patch 2 on the Krilliac/wine fork.** PR #62 is
   open and intended to be reviewed alongside #61 and #63. The
   `claude/wine-stack-pthread-refresh` branch should stay as-is.
   Earlier drafts of this file suggested closing / renaming it — that
   advice is rescinded.

## Reference: test harness

A minimal standalone program that proves `wrgsbase` works where
`arch_prctl(ARCH_SET_GS)` doesn't:

```c
/* gs-test.c — verify that wrgsbase sets gs.base on the host */
#include <stdio.h>
int main(void) {
    char buf[64] = {0};
    ((unsigned long*)buf)[6] = 0xCAFEBABE12345678UL; /* offset 0x30 */
    __asm__ volatile("wrgsbase %0" :: "r"(buf));
    unsigned long got = 0;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(got));
    printf("%%gs:0x30 = 0x%lx (expected 0xcafebabe12345678)\n", got);
    return got == 0xCAFEBABE12345678UL ? 0 : 1;
}
```

Build with `gcc gs-test.c -o gs-test` and run. On gVisor this prints
the correct sentinel and exits 0, proving wrgsbase works. On older
Linux kernels with CR4.FSGSBASE cleared, the `wrgsbase` instruction
traps with SIGILL and the program dies — that's the case where the
helper's arch_prctl fallback matters.

## Key files in the Wine source

- `dlls/ntdll/unix/signal_x86_64.c`:
  - `set_gs_base()` — the new helper (patch 3 adds this)
  - `init_handler()` — late-fixup path
  - `check_invalid_gsbase()` — copy-protection workaround path
  - `init_syscall_frame()` — the critical primary path
- `dlls/ntdll/loader.c::loader_init()` — where the original fault
  manifested
- `dlls/ntdll/unix/virtual.c::init_teb()` — where `teb->Peb = peb`
  and `teb->Tib.Self = &teb->Tib` are actually set correctly (not
  the bug — Wine's TEB init is fine)
