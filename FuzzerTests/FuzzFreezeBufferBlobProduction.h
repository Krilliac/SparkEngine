/**
 * @file FuzzFreezeBufferBlobProduction.h
 * @brief C ABI adapter for the save-state blob reader (Spark::FreezeSystem::ThawAll).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Thaw @p data through the shipped FreezeSystem and FreezeBuffer read primitives.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzThawFreezeBlob(const std::uint8_t* data, std::size_t size);
