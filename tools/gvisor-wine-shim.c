/**
 * @file gvisor-wine-shim.c
 * @brief LD_PRELOAD shim to make Wine survive a gVisor-style signal regime
 *
 * ## Background
 *
 * Running MinGW-compiled SparkEngine binaries under Wine inside a sandbox
 * backed by gVisor (runsc), Firecracker, or any user-mode kernel that does
 * not fully populate the x86_64 ucontext trap/error fields produces an
 * infinite loop at Wine startup:
 *
 *     002c:err:seh:segv_handler Got unexpected trap 0
 *     002c:err:seh:segv_handler Got unexpected trap 0
 *     ...
 *
 * Wine 9.0's `segv_handler` (dlls/ntdll/unix/signal_x86_64.c) reads
 * `uc_mcontext.gregs[REG_TRAPNO]` and bails out with the "unexpected trap"
 * message if it is not TRAP_x86_PAGEFLT (14). Real Linux kernels populate
 * this field with the CPU trap number on every signal delivery, but the
 * runsc kernel synthesizes signals and leaves `REG_TRAPNO` at 0. The
 * faulting instruction is never repaired, so it re-executes and faults
 * again, endlessly.
 *
 * ## How the shim works
 *
 * We intercept `sigaction(SIGSEGV, ...)` via `LD_PRELOAD`. When Wine
 * registers its `SA_SIGINFO` handler for `SIGSEGV`, we swap it out for a
 * tiny trampoline that:
 *
 *   1. Forces `uc_mcontext.gregs[REG_TRAPNO] = 14` (TRAP_x86_PAGEFLT)
 *   2. Forces `uc_mcontext.gregs[REG_ERR]    = 0x6` (write + user)
 *   3. Calls Wine's real handler with the fixed-up context
 *
 * Wine then takes its normal page-fault path and can resolve the access.
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
 * ## Known limitations
 *
 * This shim only fixes the trap-number loop. Wine 9.0 also hits a
 * secondary failure under gVisor (`virtual_setup_exception stack overflow
 * 64 bytes`) when trying to build the Windows-side exception frame,
 * because gVisor places thread stacks at different addresses than Wine
 * expects. That second issue would need a Wine source patch to fix.
 *
 * ## References
 *
 *   - Wine source: dlls/ntdll/unix/signal_x86_64.c::segv_handler
 *   - .claude/knowledge/wine-gvisor-incompatibility.md
 *   - https://github.com/google/gvisor/issues/3130
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <ucontext.h>

typedef int (*sigaction_fn)(int, const struct sigaction*, struct sigaction*);

/* Pointer to Wine's real SIGSEGV handler, captured at install time. */
static void (*g_wine_segv_handler)(int, siginfo_t*, void*) = NULL;

/* Trampoline: fix up the ucontext trap fields, then chain to Wine's
 * handler. Wine's handler reads uc_mcontext.gregs[REG_TRAPNO] directly,
 * so setting that field is sufficient — no need to translate the signal. */
static void trampoline(int sig, siginfo_t* info, void* uctx)
{
    ucontext_t* uc = (ucontext_t*)uctx;
#ifdef __x86_64__
    /* Linux's <sys/ucontext.h> defines REG_TRAPNO = 20 and REG_ERR = 19
     * on x86_64. These are indices into gregs[]. */
    uc->uc_mcontext.gregs[REG_TRAPNO] = 14; /* TRAP_x86_PAGEFLT */
    uc->uc_mcontext.gregs[REG_ERR] = 0x6;   /* user write fault */
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
