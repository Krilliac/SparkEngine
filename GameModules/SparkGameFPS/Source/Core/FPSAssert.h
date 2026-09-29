/**
 * @file FPSAssert.h
 * @brief SparkGameFPS debug-only invariant checks without the engine-private Utils/Assert.h.
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

#if defined(_DEBUG) || defined(DEBUG)
#define FPS_ASSERT(expr) assert(expr)
#define FPS_ASSERT_MSG(expr, message) assert((expr) && (message))
#else
#define FPS_ASSERT(expr) ((void)0)
#define FPS_ASSERT_MSG(expr, message) ((void)0)
#endif
