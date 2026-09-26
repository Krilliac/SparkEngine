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
 */

#include "Utils/CrashHandler.h"

#include <cstdio>

// Keep the fault in its own frame so the exact PC maps to this function.
// Sanitizer instrumentation is disabled here only: under ASan/UBSan the null
// store would otherwise be reported and exit(1) before the real SIGSEGV reaches
// the crash handler, so the canary would fail in sanitizer build trees.
extern "C" __attribute__((noinline, no_sanitize("address", "undefined"))) void SparkSymbolicationCanaryCrashSite(
    volatile int* target)
{
    *target = 0x5a; // SPARK_SYMBOLICATION_CANARY_FAULT_LINE
}

int main()
{
    CrashConfig config;
    config.dumpPrefix = L"SymbolicationCanary";
    config.captureScreenshot = false;
    config.captureSystemInfo = false;
    config.captureAllThreads = false;
    config.headlessMode = true;
    InstallCrashHandler(config);

    // Opaque to the optimizer, so the store is not folded into a trap.
    volatile int* volatile target = nullptr;
    SparkSymbolicationCanaryCrashSite(target);

    // Reaching this line means the fault did not happen.
    std::fputs("CrashSymbolicationProbe: expected SIGSEGV did not occur\n", stderr);
    return 3;
}
