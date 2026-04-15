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
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <ucontext.h>
#include <unistd.h>

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
 * fix). 16 entries is enough — we care about main thread + a handful of
 * auxiliary threads during early startup. */
#define MAX_KNOWN_TEBS 16
static volatile unsigned long g_known_tebs[MAX_KNOWN_TEBS] = {0};
static volatile int g_known_tebs_count = 0;

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
 * which it misses. Off by default; set SPARK_WINE_GVISOR_SHIM_VERBOSE=1. */
static int g_trampoline_verbose = 0;

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
    if (g_fix_rsp_enabled)
    {
        unsigned long stack_base = 0;
        unsigned long stack_limit = 0;
        unsigned long teb_self = 0;
        __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb_self));
        __asm__ volatile("movq %%gs:0x8, %0" : "=r"(stack_base));
        __asm__ volatile("movq %%gs:0x10, %0" : "=r"(stack_limit));
        unsigned long old_rsp = (unsigned long)uc->uc_mcontext.gregs[REG_RSP];

        /* Wine PR #63 init_handler safety net: a fault on a thread whose
         * init_syscall_frame hasn't run yet leaves gs.base pointing at a
         * glibc-private region (where TIB.Self != self). Try each of the
         * TEBs we've previously seen; the right one is the TEB whose
         * StackBase contains the current rsp. */
        if (teb_self == 0 || teb_self > 0x800000000000UL)
        {
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
                    break;
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
         * userspace pointer (not 0, not in the kernel half). */
        if (stack_base != 0 && (stack_base & 0xFFF) == 0 && stack_base < 0x800000000000UL)
        {
            /* Use a per-thread bump counter so cascading faults don't
             * overwrite each other's exception frames. Each new fault on the
             * same thread gets a fresh 64 KiB region 64 KiB below the
             * previous one. After ~8 cascades we cover 512 KiB which is
             * more than enough for any reasonable exception dispatch chain. */
            static __thread unsigned int bump_count = 0;
            unsigned long new_rsp = (stack_base - 0x80000 - ((unsigned long)bump_count * 0x10000)) & ~0xFUL;
            ++bump_count;
            /* Always intervene when the saved RSP is below stack_limit (in
             * the guard region) OR within 32 KiB of the bottom of any
             * 2 MiB-aligned region. The check covers both "Wine's TEB
             * thinks the stack is here" (use TEB bounds directly) and
             * "the saved RSP is in some other low-stack mapping" (use
             * 2 MiB heuristic). */
            int bump = 0;
            if (stack_limit && old_rsp < stack_limit + 0x4000)
                bump = 1;
            unsigned long region_bottom = old_rsp & ~0x1FFFFFUL;
            if (old_rsp - region_bottom < 0x8000)
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
     * where a worker thread faults before its own arch_prctl runs. */
    remember_teb(teb);

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

/* Constructor: probe wrgsbase availability before Wine starts so we can
 * decide whether the syscall fallback is safe. */
__attribute__((constructor))
static void shim_init(void)
{
    g_wrgsbase_usable = probe_wrgsbase_usable();
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
