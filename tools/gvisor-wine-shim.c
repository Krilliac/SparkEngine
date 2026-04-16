/**
 * @file gvisor-wine-shim.c
 * @brief LD_PRELOAD shim that injects Wine PRs #61 and #63 into stock Wine
 *
 * ## Background
 *
 * Running MinGW-compiled SparkEngine binaries under stock Wine inside a
 * sandbox backed by gVisor (runsc), Firecracker, or any user-mode kernel
 * that imperfectly emulates the x86_64 signal/syscall regime produces two
 * distinct early-init failures:
 *
 *  1. **Infinite "Got unexpected trap 0" loop in Wine's segv_handler.**
 *     Wine reads `uc_mcontext.gregs[REG_TRAPNO]` to dispatch fault
 *     handling. gVisor synthesizes signals without populating that field,
 *     so Wine sees `trap_no == 0`, hits the `default` arm, and returns
 *     from the handler without repairing the fault. The faulting
 *     instruction re-executes and faults again, forever. Documented in
 *     `.claude/knowledge/wine-gvisor-incompatibility.md`. The upstream
 *     fix lives in `wine-mirror/wine#61`.
 *
 *  2. **`gs.base` left at a glibc-private address.** Wine's
 *     `init_syscall_frame` calls `syscall(SYS_arch_prctl, ARCH_SET_GS, teb)`
 *     to point `gs.base` at the freshly-allocated Windows TEB. gVisor's
 *     runsc sentry returns `-1` (or, on some versions, "success" without
 *     actually writing the MSR), leaving `gs.base` at whatever the
 *     emulator initially chose. Every subsequent `%gs:0x30` read in PE
 *     ntdll (which is how `NtCurrentTeb()` is implemented on x86_64
 *     Linux) returns garbage, and `loader_init` faults on a NULL-derived
 *     pointer write — manifesting downstream as a cascade of SIGSEGVs
 *     that eventually overflow the Windows-side stack and abort the
 *     thread with `virtual_setup_exception stack overflow N bytes`.
 *     Documented in `.claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md`.
 *     The upstream fix lives in `wine-mirror/wine#63`.
 *
 * Both upstream PRs are open but unmerged. The Wine maintainers may
 * never accept them, or may take months. **This shim implements both
 * fixes from outside Wine, via LD_PRELOAD interposition** of the two
 * libc symbols Wine actually calls (`sigaction` and `syscall`). No
 * Wine source changes, no Wine rebuild, no patched Wine binary — works
 * with whatever Wine the system package manager installed.
 *
 * ## How the shim works
 *
 * ### Fix 1 — Wine PR #61 (trap-no loop) via `sigaction` interception
 *
 * When Wine registers its `SA_SIGINFO` handler for `SIGSEGV`, our
 * `sigaction` wrapper swaps it out for a trampoline that:
 *   1. Forces `uc_mcontext.gregs[REG_TRAPNO] = 14` (TRAP_x86_PAGEFLT)
 *   2. Forces `uc_mcontext.gregs[REG_ERR]    = 0x6` (write + user)
 *   3. Calls Wine's real handler with the fixed-up context
 *
 * Wine then takes its normal page-fault path and can resolve the access.
 *
 * ### Fix 2 — Wine PR #63 (`set_gs_base`) via `syscall` interception
 *
 * Wine's `__wine_unix_call_dispatcher` sets `gs.base` for each Windows
 * thread by calling `syscall(SYS_arch_prctl, ARCH_SET_GS, teb)` through
 * the libc PLT. We interpose `syscall()` and, when we see that exact
 * combination, do what Wine PR #63 would do natively:
 *
 *   1. Forward the call to the real `syscall()` to give the kernel a
 *      chance (it works on every host except gVisor and friends).
 *   2. Verify the write took effect by reading `%gs:0x30`. On a
 *      correctly-initialised TEB, that equals `teb` because
 *      `teb->NtTib.Self = &teb->Tib` and `Tib` lives at offset 0
 *      inside TEB.
 *   3. If the readback doesn't match, issue `wrgsbase teb` — a direct
 *      CPU instruction, not a syscall, that the user-mode kernel
 *      cannot intercept.
 *   4. Return `0` to Wine so the unpatched call site continues into
 *      PE ntdll with the now-correct `gs.base`.
 *
 * The downstream `virtual_setup_exception stack overflow` cascade is
 * eliminated by fix 2: with `gs.base` correct, `loader_init` no longer
 * dereferences NULL, no exception cascade is generated, and the
 * Windows-side stack never gets hammered with 3776-byte frames. So
 * the two fixes together turn a hard-crash startup into a clean boot.
 *
 * ## Usage
 *
 *     gcc -shared -fPIC -O2 -o tools/gvisor-wine-shim.so \
 *         tools/gvisor-wine-shim.c -ldl
 *     LD_PRELOAD=$PWD/tools/gvisor-wine-shim.so \
 *         wine64 build/linux-mingw-release/bin/SparkEngine.exe
 *
 * Or via the tools/wine-run.sh wrapper:
 *
 *     SPARK_WINE_GVISOR_SHIM=1 tools/wine-run.sh path/to/SparkTests.exe
 *
 * ## When the wrgsbase fallback isn't safe
 *
 * `wrgsbase` requires `CR4.FSGSBASE = 1`. Real Linux kernels expose
 * this via `AT_HWCAP2 & HWCAP2_FSGSBASE`, but gVisor lies in both
 * directions — it claims the bit is unset (so a "check the bit
 * first" guard would skip the fix entirely) but the instruction
 * itself works. We therefore probe at shim init by trying `wrgsbase`
 * inside a `setjmp/SIGILL` envelope; if it traps, we leave the
 * arch_prctl call alone and fall back to the no-fix-available path.
 * On any host that exposes FSGSBASE to user mode, this is a no-op.
 *
 * ## References
 *
 *   - Wine source: dlls/ntdll/unix/signal_x86_64.c::segv_handler
 *   - Wine source: dlls/ntdll/unix/signal_x86_64.c::init_syscall_frame
 *   - Wine PR #61 — trap-siginfo fallback (matches sigaction interception)
 *   - Wine PR #63 — set_gs_base via wrgsbase (matches syscall interception)
 *   - .claude/knowledge/wine-gvisor-incompatibility.md
 *   - .claude/knowledge/wine-gvisor-root-cause-found-2026-04-14.md
 *   - https://github.com/google/gvisor/issues/3130
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#ifndef SYS_clone3
#define SYS_clone3 435
#endif

#ifndef ARCH_SET_GS
#define ARCH_SET_GS 0x1001
#endif

typedef int (*sigaction_fn)(int, const struct sigaction*, struct sigaction*);
typedef long (*syscall_fn)(long number, ...);

/* Pointer to Wine's real SIGSEGV handler, captured at install time. */
static void (*g_wine_segv_handler)(int, siginfo_t*, void*) = NULL;

/* Cached real syscall() resolved via dlsym(RTLD_NEXT, ...). */
static syscall_fn g_real_syscall = NULL;

/* Ring buffer of TEB addresses we've seen via arch_prctl(ARCH_SET_GS, teb).
 * Used by the SIGSEGV trampoline to repair gs.base when a fault arrives on
 * a thread before its init_syscall_frame has run (Wine PR #63's init_handler
 * fix). Bumped from 16 to 64 in iteration 4 to accommodate Wine processes
 * with many worker threads — the engine's JobSystem, SparkConsole subprocess
 * IPC thread, audio thread, and network threads add up. */
#define MAX_KNOWN_TEBS 64
static volatile unsigned long g_known_tebs[MAX_KNOWN_TEBS] = {0};
static volatile int g_known_tebs_count = 0;

/* Forward declaration — defined further down but referenced from the
 * signal-safe helpers that themselves appear ahead of the config block. */
static int g_trampoline_verbose;

static void remember_teb(unsigned long teb)
{
    if (teb == 0) return;
    int n = g_known_tebs_count;
    for (int i = 0; i < n && i < MAX_KNOWN_TEBS; ++i)
    {
        if (g_known_tebs[i] == teb) return; /* already recorded */
    }
    if (n < MAX_KNOWN_TEBS)
    {
        g_known_tebs[n] = teb;
        g_known_tebs_count = n + 1;
    }
}

/* Async-signal-unsafe /proc/self/maps scan to discover Wine thread TEBs that
 * we haven't seen via the arch_prctl interception path yet. Should only be
 * called from user-mode code (syscall interposer), NOT from inside a signal
 * handler. For every writable mapping, read offset 0x30 (TIB.Self) and check
 * whether it equals the mapping's own base address — that's the invariant
 * every correctly-initialised TEB satisfies. Also check that the mapping's
 * offset 0x08 (StackBase) and 0x10 (StackLimit) form a plausible stack
 * range (StackLimit < StackBase, both nonzero, both page-aligned). */
static void scan_maps_for_tebs(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        unsigned long start = 0, end = 0;
        char perms[5] = {0};
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3) continue;
        if (perms[1] != 'w') continue;
        unsigned long size = end - start;
        /* Upper limit bumped from 0x100000 to 0x1000000 to handle the case
         * where Linux coalesces many adjacent anonymous mmaps (TEBs + their
         * stacks + scratch regions) into a single /proc/self/maps entry. We
         * then scan each page inside the entry for the TIB.Self invariant. */
        if (size < 0x1000 || size > 0x1000000) continue;

        /* Walk the region page by page — see find_teb_for_rsp_signal_safe
         * for rationale. */
        for (unsigned long page = start; page + 0x38 <= end; page += 0x1000)
        {
            volatile unsigned long *candidate = (volatile unsigned long *)page;
            if (candidate[6] != page) continue;
            unsigned long stack_base = candidate[1];
            unsigned long stack_limit = candidate[2];
            if (stack_base == 0 || stack_limit == 0) continue;
            if (stack_limit >= stack_base) continue;
            if ((stack_base & 0xFFF) != 0 || (stack_limit & 0xFFF) != 0) continue;
            remember_teb(page);
        }
    }
    fclose(f);
}

/* ============================================================================
 *  Signal-safe /proc/self/maps walk (iteration 4)
 * ============================================================================
 *
 * The non-signal-safe scan above uses fopen/fgets/sscanf which may allocate,
 * take locks, or otherwise be async-signal-unsafe. For the race where a fault
 * arrives on a Wine thread whose TEB Wine allocated *after* our last scan but
 * *before* the thread's first arch_prctl ran, we need to walk /proc/self/maps
 * from inside the SIGSEGV trampoline. That means strictly signal-safe code:
 * raw open()/read()/close() syscalls, manual hex parsing, no libc stdio, no
 * malloc, no shared mutable state except __thread (which uses fs.base and is
 * always safe even when gs.base is the reason we're in the handler).
 *
 * Per POSIX, `open`, `read`, and `close` are async-signal-safe, so the libc
 * wrappers are fine. We avoid FILE*, fgets, fscanf, sscanf, and malloc. */

static ssize_t read_proc_maps_signal_safe(char *buf, size_t bufsz)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t total = 0;
    while ((size_t)total + 1 < bufsz)
    {
        ssize_t n = read(fd, buf + total, bufsz - 1 - (size_t)total);
        if (n < 0)
        {
            /* EINTR — retry; any other error — bail. */
            if (n == -1) { /* fallthrough to break on any negative */ }
            break;
        }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    if (total < 0) total = 0;
    buf[total] = 0;
    return total;
}

/* Parse one /proc/self/maps line from buf+0 up to the first newline or EOF.
 * Fills *out_start, *out_end, out_perms[0..3]. Returns the number of bytes
 * consumed (including the newline). On a malformed line, still advances past
 * the newline and returns the consumed byte count so the caller can continue. */
static size_t parse_maps_line(const char *buf, size_t len,
                              unsigned long *out_start, unsigned long *out_end,
                              char out_perms[4])
{
    size_t i = 0;
    *out_start = 0;
    *out_end = 0;
    out_perms[0] = out_perms[1] = out_perms[2] = out_perms[3] = 0;

    /* start (hex) */
    while (i < len)
    {
        char c = buf[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10u + (unsigned)(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10u + (unsigned)(c - 'A');
        else break;
        *out_start = (*out_start << 4) | d;
        ++i;
    }
    if (i >= len || buf[i] != '-') goto eol;
    ++i;

    /* end (hex) */
    while (i < len)
    {
        char c = buf[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = 10u + (unsigned)(c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10u + (unsigned)(c - 'A');
        else break;
        *out_end = (*out_end << 4) | d;
        ++i;
    }
    if (i >= len || buf[i] != ' ') goto eol;
    ++i;

    /* perms — exactly four chars like "rw-p". */
    if (i + 4 > len) goto eol;
    out_perms[0] = buf[i++];
    out_perms[1] = buf[i++];
    out_perms[2] = buf[i++];
    out_perms[3] = buf[i++];

eol:
    while (i < len && buf[i] != '\n') ++i;
    if (i < len) ++i; /* consume newline */
    return i;
}

/* Signal-safe: walk /proc/self/maps looking for a Wine TEB whose stack range
 * [StackLimit, StackBase] contains target_rsp. Returns the TEB base address
 * or 0 on failure. Side-effect: every valid TEB found is recorded in
 * g_known_tebs[] so subsequent faults can match without a rescan.
 *
 * Buffer is a plain stack local rather than __thread because __thread inside
 * a shared library is "global-dynamic" TLS by default, and first-time access
 * from a signal handler goes through `__tls_get_addr` which is NOT
 * async-signal-safe. A stack local is trivially signal-safe (each frame has
 * its own copy), and 16 KiB fits easily in any sane thread stack — even
 * Wine's Windows-side stacks, which are typically 1 MiB. 16 KiB is enough
 * for /proc/self/maps on a medium-sized Wine process (~100 regions); larger
 * processes may truncate, but truncation just means we scan fewer entries,
 * which is still strictly better than zero entries. */
/* write() a short async-signal-safe banner for debugging — unlike fprintf,
 * write() is on POSIX's AS-safe list. Not a hot path; only compiled in when
 * SPARK_WINE_GVISOR_SHIM_VERBOSE is set at runtime. */
static void as_safe_puts(const char *s)
{
    size_t len = 0;
    while (s[len]) ++len;
    (void)!write(2, s, len);
}
static void as_safe_hex(unsigned long v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; ++i)
    {
        unsigned nib = (v >> ((15 - i) * 4)) & 0xF;
        buf[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    buf[18] = 0;
    as_safe_puts(buf);
}

static unsigned long find_teb_for_rsp_signal_safe(unsigned long target_rsp)
{
    char buf[16 * 1024];
    ssize_t total = read_proc_maps_signal_safe(buf, sizeof(buf));
    if (g_trampoline_verbose)
    {
        as_safe_puts("[gvisor-shim][find] read total=");
        as_safe_hex((unsigned long)total);
        as_safe_puts(" target_rsp=");
        as_safe_hex(target_rsp);
        as_safe_puts("\n");
    }
    if (total <= 0) return 0;

    size_t pos = 0;
    int rw_matches = 0;
    int tib_self_matches = 0;
    while (pos < (size_t)total)
    {
        unsigned long start = 0, end = 0;
        char perms[4] = {0};
        size_t advance = parse_maps_line(buf + pos, (size_t)total - pos,
                                         &start, &end, perms);
        if (advance == 0) break;
        pos += advance;

        if (start == 0 || end <= start) continue;
        /* TEBs are writable. */
        if (perms[1] != 'w') continue;
        unsigned long size = end - start;
        /* Upper-size filter only — Linux coalesces adjacent anonymous mmaps
         * with identical permissions into single /proc/self/maps entries, so
         * a TEB can live at any page offset inside a larger coalesced rw
         * region. Accept regions as large as 16 MiB and scan each page
         * individually for the TIB.Self invariant. Below 4 KiB there's no
         * room for a Wine TEB so skip. */
        if (size < 0x1000 || size > 0x1000000) continue;
        ++rw_matches;

        /* Walk the region page by page. A Wine TEB is always page-aligned
         * and its Tib.Self (offset 0x30) equals the TEB base. Scanning every
         * page within a coalesced region is O(region_size / 4K) which is
         * cheap compared to the signal overhead of getting here. */
        for (unsigned long page = start; page + 0x38 <= end; page += 0x1000)
        {
            volatile unsigned long *teb = (volatile unsigned long *)page;
            if (teb[6] != page) continue;
            ++tib_self_matches;

            unsigned long stack_base  = teb[1]; /* offset 0x08 */
            unsigned long stack_limit = teb[2]; /* offset 0x10 */
            if (g_trampoline_verbose)
            {
                as_safe_puts("[gvisor-shim][find] candidate TEB=");
                as_safe_hex(page);
                as_safe_puts(" sb=");
                as_safe_hex(stack_base);
                as_safe_puts(" sl=");
                as_safe_hex(stack_limit);
                as_safe_puts("\n");
            }
            if (!stack_base || !stack_limit) continue;
            if (stack_limit >= stack_base) continue;
            if ((stack_base  & 0xFFF) != 0) continue;
            if ((stack_limit & 0xFFF) != 0) continue;

            /* Cache for next time so we can short-circuit future faults. */
            remember_teb(page);

            /* The payload: does this TEB's stack range bracket the current rsp? */
            if (target_rsp >= stack_limit && target_rsp <= stack_base)
            {
                if (g_trampoline_verbose)
                {
                    as_safe_puts("[gvisor-shim][find] MATCH rsp in range\n");
                }
                return page;
            }
        }
    }
    if (g_trampoline_verbose)
    {
        as_safe_puts("[gvisor-shim][find] no match (rw regions=");
        as_safe_hex((unsigned long)rw_matches);
        as_safe_puts(" TIB.Self matches=");
        as_safe_hex((unsigned long)tib_self_matches);
        as_safe_puts(")\n");
    }
    return 0;
}

/* Whether the wrgsbase instruction is usable on this host. Probed once at
 * shim init via a SIGILL envelope so we never crash on hosts that disable
 * CR4.FSGSBASE. -1 = not yet probed, 0 = unsafe, 1 = safe. */
static int g_wrgsbase_usable = -1;

/* Opt-in for the SIGSEGV trampoline RSP-bump fix (Wine PR #62 bypass).
 * Default off — only enable when the user knows the gVisor cascade is the
 * problem they're trying to escape. Set via SPARK_WINE_GVISOR_FIX_RSP=1. */
static int g_fix_rsp_enabled = 0;

/* Verbose trampoline logging — prints rsp, gs.base bounds, and si_addr on
 * every SIGSEGV so we can diagnose which faults the heuristic catches and
 * which it misses. Off by default; set SPARK_WINE_GVISOR_SHIM_VERBOSE=1.
 * (Forward-declared near the top of the file so signal-safe helpers that
 * appear above this block can reference it.) */

/* SIGILL probe envelope for wrgsbase. */
static sigjmp_buf g_sigill_env;
static volatile sig_atomic_t g_in_sigill_probe = 0;
static void sigill_probe_handler(int sig)
{
    (void)sig;
    if (g_in_sigill_probe)
        siglongjmp(g_sigill_env, 1);
}

/* Probe whether the wrgsbase instruction is usable on this host. Saves and
 * restores the current gs.base via a sentinel write. Returns 1 if usable,
 * 0 if it traps with SIGILL or behaves wrong. */
static int probe_wrgsbase_usable(void)
{
    struct sigaction old_sa, new_sa;
    memset(&new_sa, 0, sizeof(new_sa));
    new_sa.sa_handler = sigill_probe_handler;
    sigemptyset(&new_sa.sa_mask);
    new_sa.sa_flags = 0;

    /* Resolve and call the *real* sigaction so our own interposer doesn't
     * try to swap this temporary handler with Wine's SIGSEGV trampoline. */
    static sigaction_fn real_sa = NULL;
    if (!real_sa) real_sa = (sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
    if (!real_sa) return 0;

    if (real_sa(SIGILL, &new_sa, &old_sa) != 0) return 0;

    /* Save current gs.base so we can restore it after the probe. */
    unsigned long saved_gs_base = 0;
    __asm__ volatile("rdgsbase %0" : "=r"(saved_gs_base));

    volatile int ok = 0;
    g_in_sigill_probe = 1;
    if (sigsetjmp(g_sigill_env, 1) == 0)
    {
        /* Allocate a small TEB-shaped buffer and put a known sentinel at
         * the offset Wine reads (TIB.Self at +0x30). Then write gs.base to
         * point at the buffer and read it back. */
        static __thread unsigned long buf[16];
        memset(buf, 0, sizeof(buf));
        buf[6] = 0xCAFEBABEDEADBEEFUL; /* offset 0x30 */
        __asm__ volatile("wrgsbase %0" :: "r"((unsigned long)buf));
        unsigned long got = 0;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(got));
        if (getenv("SPARK_WINE_GVISOR_SHIM_VERBOSE"))
        {
            fprintf(stderr, "[gvisor-shim] probe: wrote 0x%lx, expected sentinel 0x%lx, "
                    "readback 0x%lx, saved_gs_base=0x%lx\n",
                    (unsigned long)buf, 0xCAFEBABEDEADBEEFUL, got, saved_gs_base);
        }
        if (got == 0xCAFEBABEDEADBEEFUL) ok = 1;
    }
    g_in_sigill_probe = 0;
    /* Restore the original gs.base — never leave it pointing at our scratch
     * buffer or zero, since glibc may use TLS via gs.base on some configs. */
    __asm__ volatile("wrgsbase %0" :: "r"(saved_gs_base));
    real_sa(SIGILL, &old_sa, NULL);
    return ok;
}

/* Trampoline: fix up the ucontext trap fields, optionally bump REG_RSP into
 * the middle of the active Windows thread stack to bypass Wine's
 * `virtual_setup_exception` bounds check (Wine PR #62 territory), then chain
 * to Wine's handler. */
static void trampoline(int sig, siginfo_t* info, void* uctx)
{
    ucontext_t* uc = (ucontext_t*)uctx;
    int gs_was_repaired = 0; /* Set to 1 if we fixed gs.base (Option C) */
#ifdef __x86_64__
    /* Linux's <sys/ucontext.h> defines REG_TRAPNO = 20 and REG_ERR = 19
     * on x86_64. These are indices into gregs[]. */
    uc->uc_mcontext.gregs[REG_TRAPNO] = 14; /* TRAP_x86_PAGEFLT */
    uc->uc_mcontext.gregs[REG_ERR] = 0x6;   /* user write fault */

    /* ----------------------------------------------------------------------
     * Fix 3 — Wine PR #62: virtual_setup_exception bounds-check bypass
     * ----------------------------------------------------------------------
     *
     * On gVisor, the Windows-side thread stack mapped by Wine and the
     * pthread stack the kernel actually placed the thread on can disagree.
     * When a SIGSEGV arrives during early init, the saved RSP can be inside
     * the guard page of Wine's cached stack region, and
     * `virtual_setup_exception` aborts the thread with
     * "stack overflow N bytes addr %p stack %p (%p-%p-%p)".
     *
     * We don't have a fix-up to offer the bounds check itself (the
     * function is static and the bounds live on its stack frame), so
     * instead we re-position the saved RSP to a safe mid-stack location
     * before Wine's handler reads it. Wine's handler computes
     * `new_stack = rsp - frame_size` and tests against
     * `stack_info.start + 4096`; if rsp is well above the guard, the check
     * passes and Wine builds the exception frame on virgin stack memory.
     *
     * We compute the safe location from the TEB itself (which our
     * arch_prctl interception has already pointed gs.base at). The TEB's
     * `Tib.StackBase` lives at offset 0x08 and gives us the top of the
     * active Windows thread stack; setting RSP = StackBase - 0x80000 puts
     * us 512 KiB below the top, well above any guard page, with plenty of
     * headroom for both the exception frame and the resumed exception
     * dispatcher.
     *
     * The original execution context cannot be unwound after this — but on
     * the gVisor cascade we're trying to escape, the faulting code is
     * itself a NULL/garbage deref in PE ntdll early init that has no
     * sensible stack to unwind anyway. The exception dispatcher takes over
     * and Wine progresses past the fault.
     *
     * Disabled by default; enable with `SPARK_WINE_GVISOR_FIX_RSP=1`.
     */
    /* The gs.base repair path is ALWAYS on — it's a strict improvement
     * over the cascade and has no known side-effects on normal Wine
     * execution. The RSP-bump path is opt-in via SPARK_WINE_GVISOR_FIX_RSP
     * because it can corrupt SEH frame chains when it fires on faults
     * that don't need it. */
    {
        unsigned long stack_base = 0;
        unsigned long stack_limit = 0;
        unsigned long teb_self = 0;
        unsigned long gs_base = 0;
        __asm__ volatile("rdgsbase %0" : "=r"(gs_base));
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb_self));
        __asm__ volatile("movq %%gs:0x8, %0" : "=r"(stack_base));
        __asm__ volatile("movq %%gs:0x10, %0" : "=r"(stack_limit));
        unsigned long old_rsp = (unsigned long)uc->uc_mcontext.gregs[REG_RSP];

        /* Wine PR #63 init_handler safety net: a fault on a thread whose
         * init_syscall_frame hasn't run yet leaves gs.base pointing at a
         * glibc-private region (where TIB.Self != self). Try each of the
         * TEBs we've previously seen; the right one is the TEB whose
         * StackBase contains the current rsp.
         *
         * Detect "gs.base is wrong" heuristically. The strong check would
         * be `rdgsbase == %gs:0x30` (TIB.Self invariant), but empirically
         * that's too strict on Wine 9.0 — Wine mid-init sometimes runs
         * with gs.base correct but TIB.Self not yet populated, and we
         * don't want to repair in that window. Instead, accept the
         * current gs.base as long as the stack layout fields look
         * plausible (non-NULL, valid ordering, rsp inside the range).
         * This is a heuristic but in practice catches the worker-thread-
         * faults-before-init-syscall-frame cascade without misdiagnosing
         * legitimate mid-init state. */
        int gs_looks_valid =
            (stack_base != 0 && stack_limit != 0 &&
             stack_limit < stack_base &&
             old_rsp >= stack_limit && old_rsp <= stack_base);
        if (!gs_looks_valid)
        {
            int repaired = 0;
            int n = g_known_tebs_count;
            for (int i = 0; i < n && i < MAX_KNOWN_TEBS; ++i)
            {
                unsigned long candidate = g_known_tebs[i];
                if (candidate == 0) continue;
                /* Read TIB.StackBase (offset 0x08) and TIB.StackLimit
                 * (offset 0x10) directly from the candidate TEB and check
                 * whether old_rsp falls inside that range. */
                unsigned long cand_base = *(volatile unsigned long *)(candidate + 0x08);
                unsigned long cand_lim  = *(volatile unsigned long *)(candidate + 0x10);
                if (cand_base && old_rsp <= cand_base && old_rsp >= cand_lim)
                {
                    __asm__ volatile("wrgsbase %0" :: "r"(candidate));
                    /* Re-read after wrgsbase for the diagnostic block below. */
                    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb_self));
                    __asm__ volatile("movq %%gs:0x8, %0"  : "=r"(stack_base));
                    __asm__ volatile("movq %%gs:0x10, %0" : "=r"(stack_limit));
                    static int repaired_logged = 0;
                    if (!repaired_logged || g_trampoline_verbose)
                    {
                        fprintf(stderr,
                                "[gvisor-shim] trampoline: gs.base was wrong, "
                                "repaired to TEB 0x%lx (StackBase=0x%lx)\n",
                                candidate, cand_base);
                        repaired_logged = 1;
                    }
                    repaired = 1;
                    gs_was_repaired = 1;
                    break;
                }
            }

            /* ----------------------------------------------------------------
             * Iteration 4: signal-safe /proc/self/maps fallback
             * ----------------------------------------------------------------
             *
             * If the known_tebs loop didn't find a match, this is the race
             * case: Wine allocated a TEB for a new thread *after* our last
             * scan_maps_for_tebs() call (which runs at sigaction-install and
             * arch_prctl-interception times), but *before* the thread's own
             * init_syscall_frame ran — so the new TEB isn't in g_known_tebs[]
             * yet. Do a fresh /proc/self/maps walk from inside the signal
             * handler, signal-safely, looking for a TEB whose stack range
             * contains the current rsp. This is the thing that "blocks" the
             * thread at its fault and tells Wine that gs.base is "fully
             * acquired" before chaining to Wine's init_handler.
             *
             * Note: find_teb_for_rsp_signal_safe also calls remember_teb()
             * on every valid TEB it encounters, so the next fault on ANY
             * thread will short-circuit via the fast known_tebs path above.
             */
            if (!repaired)
            {
                unsigned long rescued = find_teb_for_rsp_signal_safe(old_rsp);
                if (rescued)
                {
                    __asm__ volatile("wrgsbase %0" :: "r"(rescued));
                    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb_self));
                    __asm__ volatile("movq %%gs:0x8, %0"  : "=r"(stack_base));
                    __asm__ volatile("movq %%gs:0x10, %0" : "=r"(stack_limit));
                    gs_was_repaired = 1;
                    static int rescue_logged = 0;
                    if (!rescue_logged || g_trampoline_verbose)
                    {
                        fprintf(stderr,
                                "[gvisor-shim] trampoline: gs.base was wrong "
                                "AND no cached TEB matched; rescued via "
                                "/proc/self/maps rescan to TEB 0x%lx "
                                "(StackBase=0x%lx, StackLimit=0x%lx, "
                                "rsp=0x%lx)\n",
                                rescued, stack_base, stack_limit, old_rsp);
                        rescue_logged = 1;
                    }
                }
                else if (g_trampoline_verbose)
                {
                    fprintf(stderr,
                            "[gvisor-shim] trampoline: gs.base repair failed "
                            "— no TEB in known_tebs or /proc/self/maps whose "
                            "stack range contains rsp=0x%lx. Chaining to "
                            "Wine anyway; cascade likely.\n", old_rsp);
                }
            }
        }

        if (g_trampoline_verbose)
        {
            fprintf(stderr,
                    "[gvisor-shim] trampoline: rsp=0x%lx, gs.StackBase=0x%lx, "
                    "gs.StackLimit=0x%lx, si_addr=%p\n",
                    old_rsp, stack_base, stack_limit, info ? info->si_addr : NULL);
        }

        /* Heuristic sanity: stack_base must be page-aligned and look like a
         * userspace pointer. Also require that the original rsp is inside
         * the cached TEB stack region — otherwise we're bumping a fault
         * that didn't originate on this thread's stack and would corrupt
         * unrelated code. Typical Wine 64-bit thread stacks live in the
         * upper 128 TiB range (0x7e...0x7f...), so an old_rsp of
         * ~0x1000ff660 (a wow64 thunk address) wouldn't match any real
         * stack region — we leave those faults alone. */
        unsigned long stack_span_lo = stack_base - 0x200000; /* ~2 MiB stack */
        if (g_fix_rsp_enabled &&
            stack_base != 0 && (stack_base & 0xFFF) == 0 && stack_base < 0x800000000000UL &&
            old_rsp >= stack_span_lo && old_rsp <= stack_base)
        {
            /* Use a per-thread bump counter so cascading faults don't
             * overwrite each other's exception frames. Each new fault on the
             * same thread gets a fresh 64 KiB region 64 KiB below the
             * previous one. After ~8 cascades we cover 512 KiB which is
             * more than enough for any reasonable exception dispatch chain. */
            static __thread unsigned int bump_count = 0;
            unsigned long new_rsp = (stack_base - 0x80000 - ((unsigned long)bump_count * 0x10000)) & ~0xFUL;
            ++bump_count;
            /* Intervene only when rsp is genuinely inside the guard page
             * of the cached TEB stack — i.e., below StackLimit — so
             * virtual_setup_exception would definitely fire. Being less
             * aggressive avoids corrupting the SEH frame chain during
             * normal exception dispatch. */
            int bump = 0;
            if (stack_limit && old_rsp < stack_limit + 0x200)
                bump = 1;

            if (bump)
            {
                uc->uc_mcontext.gregs[REG_RSP] = (greg_t)new_rsp;
                static int logged = 0;
                if (!logged || g_trampoline_verbose)
                {
                    fprintf(stderr,
                            "[gvisor-shim] bumped RSP 0x%lx -> 0x%lx (TEB.StackBase=0x%lx) "
                            "to bypass virtual_setup_exception bounds check\n",
                            old_rsp, new_rsp, stack_base);
                    logged = 1;
                }
            }
        }
    }
#endif
    /* -----------------------------------------------------------------------
     * Option C — Retry instead of dispatching when gs.base was repaired.
     * -----------------------------------------------------------------------
     *
     * If our trampoline repaired gs.base for this thread (either from the
     * known-TEB cache or via /proc/self/maps rescan), the faulting
     * instruction was almost certainly a TEB-relative access (mov
     * [gs:offset], reg) that produced a NULL-pointer write because
     * gs.base was garbage. Now that gs.base points at the correct TEB,
     * the same instruction will succeed on retry.
     *
     * Chaining to Wine's handler is WRONG in this case: Wine tries to
     * dispatch via SEH, but on threads that haven't finished
     * init_syscall_frame + signal_init_thread, there is no SEH chain
     * registered yet. Wine declares the exception unhandled and launches
     * winedbg, which itself loses the gs.base race and hangs forever.
     *
     * By returning from the signal handler instead of chaining, the
     * kernel restores the saved context (with gs.base now correct) and
     * re-executes the faulting instruction. This is the correct recovery
     * for the "thread faults before init_syscall_frame" case.
     *
     * We only do this when gs.base was ACTUALLY wrong and we repaired it.
     * If gs.base looked valid but the thread faulted for some other reason
     * (genuine NULL deref, real bug), we chain to Wine's handler normally
     * so SEH dispatch works as Wine expects.
     */
    if (gs_was_repaired)
    {
        /* gs.base was wrong and we repaired it via wrgsbase. Return from
         * the signal handler so the kernel restores the saved context
         * (with gs.base now correct) and re-executes the faulting
         * instruction. This is the correct recovery for the "thread
         * faults before init_syscall_frame" case — chaining to Wine's
         * handler would dispatch via SEH, but there's no SEH chain on
         * a thread that hasn't finished signal_init_thread yet, so Wine
         * would declare the exception unhandled and launch winedbg. */
        static int retry_logged = 0;
        if (!retry_logged || g_trampoline_verbose)
        {
            fprintf(stderr,
                    "[gvisor-shim] gs.base repaired — retrying faulting "
                    "instruction (addr=%p)\n",
                    info ? info->si_addr : NULL);
            retry_logged = 1;
        }
        return;
    }
    if (g_wine_segv_handler)
    {
        g_wine_segv_handler(sig, info, uctx);
    }
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact)
{
    static sigaction_fn real = NULL;
    if (!real)
    {
        real = (sigaction_fn)dlsym(RTLD_NEXT, "sigaction");
        if (!real)
        {
            fprintf(stderr, "[gvisor-shim] dlsym(sigaction) failed: %s\n", dlerror());
            abort();
        }
    }

    /* Only swap the first SA_SIGINFO handler installed for SIGSEGV. Wine
     * installs its handler once during early ntdll init; subsequent
     * installs (by libc, gdb, etc.) are left alone. */
    if (signum == SIGSEGV && act != NULL && g_wine_segv_handler == NULL && (act->sa_flags & SA_SIGINFO) != 0 &&
        act->sa_sigaction != NULL)
    {
        g_wine_segv_handler = act->sa_sigaction;
        struct sigaction wrapped = *act;
        wrapped.sa_sigaction = trampoline;
        fprintf(stderr,
                "[gvisor-shim] installed SIGSEGV trampoline "
                "(wine handler=%p)\n",
                (void*)g_wine_segv_handler);
        /* Seed our TEB cache from /proc/self/maps so the trampoline has
         * candidates to try even if a fault arrives before any arch_prctl
         * has run. Wine allocates all its initial TEBs before it installs
         * the SIGSEGV handler, so scanning at this point catches them. */
        scan_maps_for_tebs();
        if (g_trampoline_verbose)
        {
            fprintf(stderr, "[gvisor-shim] TEB cache seeded: %d known TEBs\n",
                    g_known_tebs_count);
        }
        return real(signum, &wrapped, oldact);
    }

    return real(signum, act, oldact);
}

/* ============================================================================
 *  Fix 2 — Wine PR #63: set_gs_base via syscall() interception
 * ============================================================================
 *
 * Wine's __wine_unix_call_dispatcher calls libc's syscall() to issue
 * arch_prctl(ARCH_SET_GS, teb). On gVisor that syscall returns -1 (or, on
 * other emulator versions, "success" without writing the MSR). Either way,
 * gs.base is left at a non-TEB address and PE ntdll's first NtCurrentTeb()
 * deref (loader_init) faults on a NULL-derived pointer write. We catch the
 * call here and finish the job using the wrgsbase instruction.
 */
long syscall(long number, ...)
{
    if (!g_real_syscall)
    {
        g_real_syscall = (syscall_fn)dlsym(RTLD_NEXT, "syscall");
        if (!g_real_syscall)
        {
            fprintf(stderr, "[gvisor-shim] dlsym(syscall) failed: %s\n", dlerror());
            abort();
        }
    }

    /* Pull up to 6 args off the stack — enough for any Linux x86_64 syscall.
     * We always read all six because we forward the call unconditionally
     * for everything except SYS_arch_prctl + ARCH_SET_GS. */
    va_list ap;
    va_start(ap, number);
    long a1 = va_arg(ap, long);
    long a2 = va_arg(ap, long);
    long a3 = va_arg(ap, long);
    long a4 = va_arg(ap, long);
    long a5 = va_arg(ap, long);
    long a6 = va_arg(ap, long);
    va_end(ap);

    /* Option B: intercept clone/clone3 to fix gs.base in the child
     * thread before ANY code (including Wine's start_thread) runs.
     * This catches threads created via raw clone (bypassing
     * pthread_create) which our Option A wrapper can't see.
     *
     * After clone returns 0 in the child, we rescan /proc/self/maps
     * to find the TEB for our current stack and wrgsbase it. This
     * runs before the child's first instruction after returning from
     * the syscall wrapper, closing the pre-init_syscall_frame window. */
    if (number == SYS_clone || number == SYS_clone3)
    {
        long rc = g_real_syscall(number, a1, a2, a3, a4, a5, a6);
        if (rc == 0 && g_wrgsbase_usable)
        {
            /* We're in the child. Try to fix gs.base immediately. */
            unsigned long rsp;
            __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
            unsigned long teb = find_teb_for_rsp_signal_safe(rsp);
            if (teb)
            {
                __asm__ volatile("wrgsbase %0" ::"r"(teb));
                static __thread int clone_logged = 0;
                if (!clone_logged)
                {
                    fprintf(stderr,
                            "[gvisor-shim] clone child: pre-set gs.base to "
                            "TEB 0x%lx (rsp=0x%lx)\n",
                            teb, rsp);
                    clone_logged = 1;
                }
            }
        }
        return rc;
    }

    if (number != SYS_arch_prctl || a1 != ARCH_SET_GS)
    {
        return g_real_syscall(number, a1, a2, a3, a4, a5, a6);
    }

    /* Forward to the real syscall first — on real Linux it works and we
     * return immediately without touching wrgsbase. */
    long rc = g_real_syscall(number, a1, a2, a3, a4, a5, a6);

    /* Verify whether the kernel actually wrote gs.base. On a correctly
     * initialized TEB, %gs:0x30 reads back as `addr` because TIB.Self is
     * set to &TIB at TEB allocation time and TIB lives at offset 0 inside
     * TEB. If the readback equals our intended TEB pointer, we're done. */
    unsigned long teb = (unsigned long)a2;
    unsigned long probe = 0;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(probe));
    if (probe == teb)
    {
        return rc;
    }

    /* The kernel either failed (returned -1) or lied (returned 0 but never
     * wrote the MSR). Use wrgsbase as a fallback if it's safe on this host. */
    if (g_wrgsbase_usable != 1)
    {
        static int warned = 0;
        if (!warned)
        {
            fprintf(stderr,
                    "[gvisor-shim] WARNING: arch_prctl(ARCH_SET_GS) is broken on this host "
                    "(rc=%ld, gs:0x30=0x%lx, expected 0x%lx) and wrgsbase is unsafe — "
                    "Wine startup will fail. There is no user-space fallback available.\n",
                    rc, probe, teb);
            warned = 1;
        }
        return rc;
    }

    __asm__ volatile("wrgsbase %0" :: "r"(teb));

    /* Re-verify so we can log whether the fallback actually worked. */
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(probe));

    /* Record this TEB so the SIGSEGV trampoline can recover from a race
     * where a worker thread faults before its own arch_prctl runs. Also
     * scan /proc/self/maps for any other TEBs we haven't seen yet —
     * this is what catches TEBs for threads whose init_syscall_frame
     * hasn't been reached yet, or which Wine creates without going
     * through our libc-interposed path. Re-scan on every new ARCH_SET_GS
     * call because Wine allocates TEBs lazily and the one that faults
     * later in the run might not exist yet on the first scan. */
    remember_teb(teb);
    scan_maps_for_tebs();

    static int logged = 0;
    if (!logged)
    {
        fprintf(stderr,
                "[gvisor-shim] arch_prctl(ARCH_SET_GS) silently broken — "
                "fell back to wrgsbase (teb=0x%lx, gs:0x30 readback=0x%lx, %s)\n",
                teb, probe, probe == teb ? "OK" : "STILL BROKEN");
        logged = 1;
    }

    /* Return success even if the kernel said -1: gs.base now points at the
     * TEB via wrgsbase, which is the only thing Wine's caller cares about. */
    return 0;
}

/* ============================================================================
 *  Fix 4 — Option A: pthread_create interception
 * ============================================================================
 *
 * Wrap every new thread's start function so we fix gs.base BEFORE any
 * Wine code executes on that thread. This closes the window between
 * clone() and init_syscall_frame where gs.base is garbage and any
 * gs:offset access faults.
 *
 * We interpose pthread_create via LD_PRELOAD, same as sigaction/syscall.
 * The wrapper allocates a small trampoline struct on the heap that
 * records the original start function and argument, then calls our
 * wrapper_start which fixes gs.base before calling the original.
 */

typedef int (*pthread_create_fn)(pthread_t*, const pthread_attr_t*,
                                 void* (*)(void*), void*);

struct pthread_wrapper_arg
{
    void* (*real_start)(void*);
    void* real_arg;
};

static void* pthread_wrapper_start(void* raw)
{
    struct pthread_wrapper_arg w = *(struct pthread_wrapper_arg*)raw;
    free(raw);

    /* Fix gs.base for this thread before any Wine code runs.
     * Walk our known-TEB cache first (fast path); fall back to
     * /proc/self/maps if no match (new TEB allocated since last scan). */
    if (g_wrgsbase_usable)
    {
        unsigned long rsp;
        __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));

        int fixed = 0;
        int n = g_known_tebs_count;
        for (int i = 0; i < n && i < MAX_KNOWN_TEBS; ++i)
        {
            unsigned long cand = g_known_tebs[i];
            if (!cand)
                continue;
            unsigned long cand_base = *(volatile unsigned long*)(cand + 0x08);
            unsigned long cand_lim = *(volatile unsigned long*)(cand + 0x10);
            if (cand_base && rsp <= cand_base && rsp >= cand_lim)
            {
                __asm__ volatile("wrgsbase %0" ::"r"(cand));
                fixed = 1;
                static int pt_logged = 0;
                if (!pt_logged || g_trampoline_verbose)
                {
                    fprintf(stderr,
                            "[gvisor-shim] pthread_create wrapper: pre-set gs.base "
                            "to TEB 0x%lx (StackBase=0x%lx) before Wine code\n",
                            cand, cand_base);
                    pt_logged = 1;
                }
                break;
            }
        }
        if (!fixed)
        {
            /* TEB not in cache yet — rescan /proc/self/maps. */
            unsigned long teb = find_teb_for_rsp_signal_safe(rsp);
            if (teb)
            {
                __asm__ volatile("wrgsbase %0" ::"r"(teb));
                static int pt_rescan_logged = 0;
                if (!pt_rescan_logged || g_trampoline_verbose)
                {
                    fprintf(stderr,
                            "[gvisor-shim] pthread_create wrapper: pre-set gs.base "
                            "via /proc/self/maps rescan to TEB 0x%lx\n",
                            teb);
                    pt_rescan_logged = 1;
                }
            }
        }
    }

    return w.real_start(w.real_arg);
}

int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                   void* (*start_routine)(void*), void* arg)
{
    static pthread_create_fn real = NULL;
    if (!real)
    {
        real = (pthread_create_fn)dlsym(RTLD_NEXT, "pthread_create");
        if (!real)
        {
            fprintf(stderr, "[gvisor-shim] dlsym(pthread_create) failed: %s\n",
                    dlerror());
            abort();
        }
    }

    /* Refresh the TEB cache right before spawning, so the wrapper_start
     * on the new thread has the best chance of finding its TEB in the
     * fast path without needing a /proc/self/maps rescan. */
    scan_maps_for_tebs();

    struct pthread_wrapper_arg* w = (struct pthread_wrapper_arg*)malloc(sizeof(*w));
    if (!w)
    {
        /* OOM — fall back to unwrapped call; the trampoline will catch it. */
        return real(thread, attr, start_routine, arg);
    }
    w->real_start = start_routine;
    w->real_arg = arg;
    return real(thread, attr, pthread_wrapper_start, w);
}

/* Constructor: probe wrgsbase availability before Wine starts so we can
 * decide whether the syscall fallback is safe. */
__attribute__((constructor))
static void shim_init(void)
{
    g_wrgsbase_usable = probe_wrgsbase_usable();
    /* RSP-bump fix is opt-in because it can corrupt SEH frame chains when
     * it fires on faults that don't need it. The gs.base repair path, by
     * contrast, is always on — it's a strict improvement. */
    g_fix_rsp_enabled = (getenv("SPARK_WINE_GVISOR_FIX_RSP") != NULL);
    g_trampoline_verbose = (getenv("SPARK_WINE_GVISOR_SHIM_VERBOSE") != NULL);
    if (g_trampoline_verbose)
    {
        fprintf(stderr, "[gvisor-shim] init: wrgsbase %s, RSP-bump fix %s\n",
                g_wrgsbase_usable ? "usable (Wine PR #63 fix active)"
                                  : "unsafe (no syscall fallback)",
                g_fix_rsp_enabled ? "enabled (Wine PR #62 bypass)" : "disabled");
    }
}
