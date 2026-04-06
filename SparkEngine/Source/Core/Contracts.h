/**
 * @file Contracts.h
 * @brief C++26 contract bridge macros (preconditions, postconditions, invariants)
 *
 * Provides SPARK_EXPECTS, SPARK_ENSURES, and SPARK_INVARIANT macros that
 * forward to C++26 contract attributes ([[pre:]], [[post:]], [[assert:]])
 * when the compiler supports them (SPARK_HAS_CONTRACTS), and fall back to
 * debug-mode assertions via Assert.h otherwise.
 *
 * Zero cost in Release builds (matching existing ASSERT behavior).
 *
 * @see Assert.h, Platform.h (SPARK_HAS_CONTRACTS)
 */

#pragma once

#include "Platform.h"
#include "Utils/Assert.h"

// ============================================================================
// Contract macros — C++26 forward-compatible
//
// When C++26 contracts are available, these expand to standard attributes.
// Until then, they expand to ASSERT_MSG in debug builds and nothing in release.
//
// Usage:
//   void SetHealth(int hp)
//   {
//       SPARK_EXPECTS(hp >= 0);
//       SPARK_EXPECTS(hp <= m_maxHealth);
//       m_health = hp;
//       SPARK_ENSURES(m_health == hp);
//   }
//
//   void Tick()
//   {
//       SPARK_INVARIANT(m_initialized);
//       // ...
//   }
// ============================================================================

#if defined(SPARK_HAS_CONTRACTS)

// C++26 contracts available — use standard attributes.
// Note: [[pre:]] and [[post:]] are function-level attributes in the standard,
// but for body-level usage we use [[assert:]] uniformly. When compilers ship
// full contract support with function-declaration syntax, these macros can
// be revisited to use [[pre:]] / [[post:]] in their proper position.
#define SPARK_EXPECTS(expr) [[assert : expr]]
#define SPARK_ENSURES(expr) [[assert : expr]]
#define SPARK_INVARIANT(expr) [[assert : expr]]

#elif defined(_DEBUG) || defined(DEBUG)

// Debug fallback — fire assertion with diagnostic context
#define SPARK_EXPECTS(expr) ASSERT_MSG(expr, "Precondition violated: %s", #expr)
#define SPARK_ENSURES(expr) ASSERT_MSG(expr, "Postcondition violated: %s", #expr)
#define SPARK_INVARIANT(expr) ASSERT_MSG(expr, "Invariant violated: %s", #expr)

#else

// Release — zero cost
#define SPARK_EXPECTS(expr) ((void)0)
#define SPARK_ENSURES(expr) ((void)0)
#define SPARK_INVARIANT(expr) ((void)0)

#endif
