/**
 * @file FuzzPluginMetadataProduction.h
 * @brief C ABI adapter for the dynamic-plugin metadata gate (Spark::ValidatePluginMetadata).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Validate @p data as a plugin's .sparkplugin.json through the shipped gate and check the verdict.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzValidatePluginMetadata(const std::uint8_t* data, std::size_t size);
