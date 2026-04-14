# Wine upstream patches for gVisor / user-mode Linux compatibility

This directory holds patches against Wine that fix real bugs we hit
running MinGW-cross-compiled SparkEngine binaries under Wine inside a
gVisor-backed sandbox. PR #470 on this repo already landed two earlier
patches that were merged back into `Working`. This directory now holds
the one patch that's still pending upstream submission:

| # | File | Target | Status |
|---|------|--------|--------|
| 3 | `0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch` | `dlls/ntdll/unix/signal_x86_64.c` | **Verified end-to-end**, ready to submit |

Patches 1 and 2 from the previous session (trap-siginfo fallback and
pthread stack refresh) are on `Krilliac/wine` branches
`claude/wine-trap-siginfo-fallback` and `claude/wine-stack-pthread-refresh`.
**Patch 2 should be withdrawn** — follow-up investigation (see
`.claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md`)
proved the diagnosis wrong. Patch 3 supersedes it.

## What patch 0003 fixes

**Root cause:** under gVisor's runsc sentry (and possibly other
user-mode Linux kernels), `arch_prctl(ARCH_SET_GS, teb)` silently
fails — the syscall returns -1 or success depending on the exact
gVisor version, but the `gs.base` MSR is never written. Wine's PE
`ntdll` then reads `NtCurrentTeb()` via `%gs:0x30` and gets garbage,
which cascades into a NULL-pointer write in `loader_init` at
`peb->LdrData = &ldr`.

**Fix:** Wine currently writes gs.base via `arch_prctl(ARCH_SET_GS)`
in three places. Replace all three with a `set_gs_base()` helper that
uses the `wrgsbase` instruction (a direct CPU instruction, not a
syscall, cannot be intercepted), verifies the write took effect via
`mov %gs:0x30`, and falls back to `arch_prctl` only if the read-back
mismatches.

**Verified working:** after this patch, a minimal MinGW-compiled
`hello` program runs under Wine inside gVisor, prints its output, and
returns the correct exit code. Before the patch it either loops
forever or crashes the Wine loader.

## Applying the patch

```bash
cd /path/to/your/wine-fork
git checkout -b fix/wrgsbase wine-9.0
git am /path/to/SparkEngine/docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch
```

Or with `patch -p1` if your tree doesn't match the exact base:

```bash
patch -p1 < /path/to/SparkEngine/docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch
git add dlls/ntdll/unix/signal_x86_64.c
git commit -F <(sed -n '/^Subject:/,/^---$/p' /path/to/SparkEngine/docs/wine-upstream/0003-ntdll-use-wrgsbase-when-arch_prctl-is-broken.patch | head -n -1)
```

## Building a patched Wine

```bash
sudo apt install -y flex bison libncurses-dev
cd /path/to/wine-fork
mkdir build && cd build
../configure --prefix=$HOME/wine-patched --enable-win64 --disable-tests \
             --without-freetype --without-gstreamer
make -j$(nproc)
sudo make install  # or set PATH to $HOME/wine-patched/bin
```

`flex` is required. In a sandboxed environment where `apt-get install`
can't reach the distro mirror, `apt-get download flex libfl-dev libfl2 m4`
followed by `sudo dpkg -i *.deb` works as a fallback.

Wine builds cleanly under `-Wall -Wextra -Werror` with no new warnings
from this patch.

## Verification test

Minimal hello program:

```c
/* hello.c */
#include <stdio.h>
int main(void) {
    puts("hello from wine");
    return 42;
}
```

```bash
x86_64-w64-mingw32-gcc hello.c -o hello.exe
$HOME/wine-patched/bin/wine hello.exe; echo "rc=$?"
```

Expected output **after** patch 3 is applied:

```
hello from wine
rc=42
```

Expected output **before** (unpatched Wine running inside gVisor):

```
(loops on SIGSEGV si_addr=0x2000 forever, eventually killed by
 virtual_setup_exception, rc != 42)
```

On a real Linux kernel (laptop, VM, real hardware, `docker run
--runtime=runc`), the patch is a no-op — `wrgsbase` and `arch_prctl`
both set gs.base correctly to the TEB, so the primary path succeeds
and the arch_prctl fallback is never reached. The fix is also safe on
hosts that disable `CR4.FSGSBASE` (the `wrgsbase` instruction traps
with `SIGILL` or is silently ignored — the verifying `mov %gs:0x30`
detects this and the `arch_prctl` fallback runs, preserving the
pre-patch behaviour).

## Upstream submission notes

Submit to `wine-devel@winehq.org` as two separate patches:

1. `[PATCH 1/2] ntdll: fall back to siginfo for trap code when REG_TRAPNO is zero`
   (patch 0001 from PR #470, already on `Krilliac/wine` as
   `claude/wine-trap-siginfo-fallback`)
2. `[PATCH 2/2] ntdll: use wrgsbase to set gs.base on Linux x86_64 when arch_prctl is broken`
   (this patch, `0003-...`)

The two are independent but together make Wine work end-to-end inside
gVisor. Patch 1 alone is useful on any user-mode Linux kernel that
shares the zero-TRAPNO signal behaviour (Firecracker, some emulators).
Patch 2 alone fixes the arch_prctl problem on any environment where
`wrgsbase` is the reliable path.

The previously-written patch `claude/wine-stack-pthread-refresh`
(virtual_setup_exception / pthread refresh) should **not** be
submitted — the diagnosis was wrong, and the fix is neither correct
nor needed once this patch is in place.

## Diagnostic breadcrumb

The full root-cause analysis, including the instrumentation sequence
that found the bug, the strace evidence, and a minimal standalone
program that proves `wrgsbase` works where `arch_prctl` doesn't, is
recorded in `.claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md`.
