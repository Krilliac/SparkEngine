/**
 * @file JoltWarningRestore.h
 * @brief Re-enables important compiler warnings after JPH_SUPPRESS_WARNINGS
 *
 * JPH_SUPPRESS_WARNINGS (from Jolt's Core.h) uses #pragma diagnostic ignored
 * which applies from point of use to end of translation unit — suppressing
 * warnings in our own code too. Include this header immediately after
 * JPH_SUPPRESS_WARNINGS in each physics .cpp to restore key diagnostics.
 */

#pragma once

// Re-enable warnings that matter for engine code correctness.
// JPH_SUPPRESS_WARNINGS disables these globally; we restore them so our own
// physics logic is still checked by the compiler.

#if defined(__clang__)

#pragma clang diagnostic warning "-Wsign-conversion"
#pragma clang diagnostic warning "-Wuninitialized"
#pragma clang diagnostic warning "-Wunused-parameter"
#pragma clang diagnostic warning "-Wfloat-equal"
#pragma clang diagnostic warning "-Wold-style-cast"
#pragma clang diagnostic warning "-Wcast-align"

#elif defined(__GNUC__)

#pragma GCC diagnostic warning "-Wsign-conversion"
#pragma GCC diagnostic warning "-Wuninitialized"
#pragma GCC diagnostic warning "-Wmaybe-uninitialized"
#pragma GCC diagnostic warning "-Wunused-parameter"

#elif defined(_MSC_VER)

#pragma warning(default : 4365) // signed/unsigned mismatch
#pragma warning(default : 5219) // implicit conversion, possible loss of data
#pragma warning(default : 4100) // unreferenced formal parameter

#endif
