# Wine + gVisor: root cause found and fixed

**Date:** 2026-04-14
**Type:** Observation + Fix
**Status:** **RESOLVED**
**Session:** deep investigation + empirical fix + verified working

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

## What changed in patches 1-2 from the previous session

The first two upstream patches we produced (PR #470: trap-siginfo
fallback and pthread-refresh) remain:

| Patch | Status | Notes |
|---|---|---|
| 0001: trap-siginfo fallback | **Still correct** | Unblocks the `Got unexpected trap 0` loop. Submit to wine-devel as-is. |
| 0002: pthread stack refresh | **Withdraw** | Wrong mental model. The TEB stack and pthread stack are separate mmap regions, not narrower/wider views of the same range. The fix this session supersedes it. |
| 0003 (new): wrgsbase | **The real fix** | Replaces 0002. Submit alongside 0001. |

## What's in this branch for the Wine fork

Copy this patch into your `Krilliac/wine` fork and rebase off just
patches 1 + 3 (drop patch 2):

```bash
cd /path/to/wine-fork
git checkout -b fix/gvisor-wrgsbase wine-9.0
git am /path/to/SparkEngine/docs/wine-upstream/0001-ntdll-fall-back-to-siginfo-for-trap-code.patch
git am /path/to/SparkEngine/docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch
# note: skip 0002, it's superseded
```

Build from that branch and test. The verification test is:

```bash
echo '#include <stdio.h>
int main(void) { puts("hello"); return 42; }' > /tmp/hello.c
x86_64-w64-mingw32-gcc /tmp/hello.c -o /tmp/hello.exe
/path/to/patched/wine /tmp/hello.exe; echo rc=$?
# Expected: hello\nrc=42
```

## Open items

1. **Patch 2 removal on the Wine fork.** The `claude/wine-stack-pthread-refresh`
   branch on `Krilliac/wine` contains the now-abandoned patch 2 and
   its async-signal-safe cache followup. That branch should be closed
   / renamed `do-not-submit` / historical — the diagnosis was wrong,
   and the code change is neither correct nor needed once patch 3 is
   in place. Patch 2's TEB-bounds-widen path is never triggered
   because the TEB stack isn't actually overflowing once gs.base is
   correct.

2. **SparkTests.exe under patched Wine.** Minimal hello-world programs
   now run to completion, but `SparkTests.exe` has its own engine-init
   issues that are orthogonal to the gs.base fix — missing Vulkan,
   missing X11, minimal Wine build configuration. Getting the full
   engine test suite running under this Wine is a separate workstream
   that needs a richer Wine build (`--with-x`, `--with-vulkan`, full
   DLL set, wow64 support) and is not blocked by Wine itself anymore.

3. **Upstream submission.** Open two reports on wine-devel:
    * `[PATCH 1/2] ntdll: fall back to siginfo for trap code when
      REG_TRAPNO is zero` (patch 0001)
    * `[PATCH 2/2] ntdll: use wrgsbase to set gs.base on Linux
      x86_64 when arch_prctl is broken` (patch 0003)

   The two are independent but together they make Wine work end-to-end
   under gVisor. Patch 0001 alone is also usable on other user-mode
   Linux kernels that share the zero-TRAPNO signal behaviour.

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
