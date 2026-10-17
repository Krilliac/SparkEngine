/**
 * @file FuzzAchievementProgressProduction.h
 * @brief C ABI adapter for the achievement progress reader (AchievementSystem::LoadFromReader).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Load @p data as saved achievement progress through the shipped reader and check the state.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadAchievementProgress(const std::uint8_t* data, std::size_t size);
