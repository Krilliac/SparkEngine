/**
 * @file FuzzEngineSettingsProduction.h
 * @brief C ABI adapter for the engine settings reader (EngineSettings::Load).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Load @p data as settings.ini (and, after the first NUL byte, settings.local.ini)
///        through the shipped EngineSettings::Load and check the result.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadEngineSettings(const std::uint8_t* data, std::size_t size);
