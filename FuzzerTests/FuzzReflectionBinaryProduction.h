/**
 * @file FuzzReflectionBinaryProduction.h
 * @brief C ABI adapter for the reflection binary field decoder (Spark::DeserializeFromBinary).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Decode @p data into a reflected record through the shipped decoder and check the record.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzDeserializeReflectionBinary(const std::uint8_t* data, std::size_t size);
