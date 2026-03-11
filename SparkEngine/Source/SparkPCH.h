/**
 * @file SparkPCH.h
 * @brief Precompiled header for SparkEngine (R9.2 — Architecture Analysis)
 *
 * Includes frequently used headers across all engine subsystems.
 * Applied via target_precompile_headers() in CMake to reduce build times.
 *
 * Guidelines:
 * - Only include stable headers that rarely change
 * - Do NOT include engine headers that are frequently modified
 * - Order: C++ standard library, platform, third-party, then stable engine headers
 */

#pragma once

// ============================================================================
// C++ Standard Library
// ============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// ============================================================================
// Platform headers
// ============================================================================

#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Third-party stable headers
// ============================================================================

#include <entt/entt.hpp>
