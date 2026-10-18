/**
 * @file FuzzMaterialLoaderProduction.h
 * @brief C ABI adapter for the .sparkmat material reader (Spark::Graphics::ParseSparkMatDefinition).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as a .sparkmat file through the shipped reader and check the definition.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzParseSparkMat(const std::uint8_t* data, std::size_t size);
