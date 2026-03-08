/**
 * @file CrashHandlerStub.cpp
 * @brief No-op fallback for TriggerCrashHandler when CrashHandler.cpp is
 *        excluded from the build (e.g. miniz not available).
 *
 * This file is only compiled when MINIZ is not found. When miniz IS available,
 * the real CrashHandler.cpp provides the full implementation.
 */

#include <cstdio>

void TriggerCrashHandler(const char* assertMsg)
{
    if (assertMsg)
        std::fprintf(stderr, "Assert (no crash handler): %s\n", assertMsg);
}
