/**
 * @file FPSAssert.h
 * @brief SDK-backed debug assertions and always-on preconditions for SparkGameFPS.
 *
 * FPS_REQUIRE_MSG and FPS_REQUIRE_NOT_NULL are always active, including Release.
 * They report through the bound SDK logger and stderr, then abort. A missing or
 * throwing logger cannot disable the precondition. Expressions are evaluated once.
 *
 * FPS_ASSERT(expr) and FPS_ASSERT_MSG(expr, message) are active exactly where the
 * engine's ASSERT/ASSERT_MSG were: builds that define _DEBUG or DEBUG (MSVC Debug).
 * There they use the standard assert(), which reports the expression (and the
 * message literal) and aborts; everywhere else they compile to nothing and do
 * not evaluate @p expr. Use an explicit check with FPS_LOG_ERROR for failures a
 * release build must survive.
 *
 * @p message must be a string literal.
 */

#pragma once

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "Core/FPSLog.h"

namespace SparkGameFPS::Detail
{
    /** @brief Report a violated precondition and terminate in every build configuration. */
    [[noreturn]] inline void FailPrecondition(const char* expression, const char* message, const char* file, int line,
                                              const char* function) noexcept
    {
        // Keep a diagnostic even before OnLoad, after OnUnload, or if the host logger throws.
        std::fprintf(stderr, "FPS PRECONDITION FAILED: %s - %s (%s:%d in %s)\n", expression, message, file, line,
                     function);
        std::fflush(stderr);
        try
        {
            FPS_LOG_ERROR("FPS PRECONDITION FAILED: {} - {} ({}:{} in {})", expression, message, file, line, function);
        }
        catch (...)
        {
            // Diagnostics must not let execution resume after a failed invariant.
        }
        std::abort();
    }
} // namespace SparkGameFPS::Detail

#define FPS_REQUIRE_MSG(expr, message)                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            ::SparkGameFPS::Detail::FailPrecondition(#expr, message, __FILE__, __LINE__, __func__);                    \
        }                                                                                                              \
    } while (0)

#define FPS_REQUIRE_NOT_NULL(ptr) FPS_REQUIRE_MSG((ptr) != nullptr, "Pointer '" #ptr "' must not be null")

#if defined(_DEBUG) || defined(DEBUG)
#define FPS_ASSERT(expr) assert(expr)
#define FPS_ASSERT_MSG(expr, message) assert((expr) && (message))
#else
#define FPS_ASSERT(expr) ((void)0)
#define FPS_ASSERT_MSG(expr, message) ((void)0)
#endif
