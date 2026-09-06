/**
 * @file CrashHandlerStub.cpp
 * @brief No-op fallbacks for CrashHandler when CrashHandler.cpp is excluded
 *        from the build (e.g. miniz not available).
 *
 * This file is only compiled when MINIZ is not found. When miniz IS available,
 * the real CrashHandler.cpp provides the full implementation.
 */

#include "CrashHandler.h"
#include "Validate.h"
#include <cstdio>

void InstallCrashHandler(const CrashConfig& /*cfg*/)
{
    SPARK_LOG_WARN(Spark::LogCategory::Core, "CrashHandler stub: no crash handler available (miniz not found)");
}

void TriggerCrashHandler(const char* assertMsg)
{
    if (assertMsg)
        std::fprintf(stderr, "Assert (no crash handler): %s\n", assertMsg);
}

void TriggerCrashReport(const char* reason)
{
    if (reason)
        std::fprintf(stderr, "Crash report (no crash handler): %s\n", reason);
}

void SetAssertCrashBehavior(bool /*shouldCrash*/)
{
    // No-op without crash handler
}
