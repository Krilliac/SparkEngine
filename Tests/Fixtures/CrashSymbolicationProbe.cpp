/**
 * @file CrashSymbolicationProbe.cpp
 * @brief OPS-100 symbolication canary: crash through the production Linux crash handler.
 *
 * Installs the real CrashHandler (headless, local artifacts only) and then
 * faults inside SparkSymbolicationCanaryCrashSite(). The handler writes the
 * crash log with its build-id symbolic-frame section into the private
 * spark_crash_<pid>_* directory under TMPDIR and re-raises SIGSEGV.
 * Tests/Tools/run_crash_symbolication_canary.py drives this process, stores
 * its split debug info by build-id and requires the fault to resolve to the
 * marked line below.
 *
 * Modes (SEC2 finding 12, the fatal-signal handler must not hang or skip):
 *   --stall-report    block the handler's best-effort stage, as a crash under a
 *                     held malloc lock would; the report watchdog must still
 *                     end the process by SIGSEGV after writing the raw log.
 *   --stack-overflow  fault by exhausting the main thread's stack; the handler
 *                     must run on its alternate stack and write the log.
 */

#include "Utils/CrashHandler.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

// Keep the fault in its own frame so the exact PC maps to this function.
// Sanitizer instrumentation is disabled here only: under ASan/UBSan the null
// store would otherwise be reported and exit(1) before the real SIGSEGV reaches
// the crash handler, so the canary would fail in sanitizer build trees.
extern "C" __attribute__((noinline, no_sanitize("address", "undefined"))) void SparkSymbolicationCanaryCrashSite(
    volatile int* target)
{
    *target = 0x5a; // SPARK_SYMBOLICATION_CANARY_FAULT_LINE
}

// Each frame keeps a live 4 KiB buffer, so the recursion overflows the stack
// long before the depth counter runs out.
extern "C" __attribute__((noinline, no_sanitize("address", "undefined"))) std::size_t SparkCanaryExhaustStack(
    std::size_t depth, volatile char* previous)
{
    volatile char frame[4096];
    frame[0] = previous ? static_cast<char>(previous[0] + 1) : 0;
    if (depth == 0)
        return 0;
    return SparkCanaryExhaustStack(depth - 1, frame) + static_cast<std::size_t>(frame[0]);
}

int main(int argc, char** argv)
{
    const bool stallReport = argc > 1 && std::strcmp(argv[1], "--stall-report") == 0;
    const bool stackOverflow = argc > 1 && std::strcmp(argv[1], "--stack-overflow") == 0;

    CrashConfig config;
    config.dumpPrefix = L"SymbolicationCanary";
    config.captureScreenshot = false;
    config.captureSystemInfo = false;
    config.captureAllThreads = false;
    config.headlessMode = true;
    InstallCrashHandler(config);
    if (stallReport)
        Spark::CrashHandlerDetail::SetSignalReportStallForTesting(true);
    if (stackOverflow)
    {
        const std::size_t sum = SparkCanaryExhaustStack(static_cast<std::size_t>(-1), nullptr);
        std::fprintf(stderr, "CrashSymbolicationProbe: the stack did not overflow (%zu)\n", sum);
        return 3;
    }

    // Opaque to the optimizer, so the store is not folded into a trap.
    volatile int* volatile target = nullptr;
    SparkSymbolicationCanaryCrashSite(target);

    // Reaching this line means the fault did not happen.
    std::fputs("CrashSymbolicationProbe: expected SIGSEGV did not occur\n", stderr);
    return 3;
}
