/**
 * @file CrashHandlerStub.cpp
 * @brief No-op fallbacks for CrashHandler when CrashHandler.cpp is excluded
 *        from the build (e.g. miniz not available).
 *
 * This file is only compiled when MINIZ is not found. When miniz IS available,
 * the real CrashHandler.cpp provides the full implementation.
 */

#include "CrashHandler.h"
#include <cstdio>

void InstallCrashHandler(const CrashConfig& /*cfg*/)
{
    // No crash handler available without miniz
}

void TriggerCrashHandler(const char* assertMsg)
{
    if (assertMsg)
        std::fprintf(stderr, "Assert (no crash handler): %s\n", assertMsg);
}

void SetAssertCrashBehavior(bool /*shouldCrash*/)
{
    // No-op without crash handler
}
