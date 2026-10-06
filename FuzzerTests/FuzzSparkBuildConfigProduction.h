/**
 * @file FuzzSparkBuildConfigProduction.h
 * @brief C ABI adapter for SparkBuild's configuration reader (ConfigManager::LoadFromStream).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Load @p data as sparkbuild.ini through the shipped reader and check the commands built from it.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadSparkBuildConfig(const std::uint8_t* data, std::size_t size);
