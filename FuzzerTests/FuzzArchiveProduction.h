/**
 * @file FuzzArchiveProduction.h
 * @brief C ABI adapter for the SparkPak production reader (SparkPakReader::Open / ReadFile).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Mount @p data as a .spk archive through the shipped reader and read every entry.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzOpenSparkPak(const std::uint8_t* data, std::size_t size);
