/**
 * @file FuzzZipListingProduction.h
 * @brief C ABI adapter for SparkBuild's ZIP member listing (ArchiveExtraction::ListZipMembers).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief List @p data as a downloaded ZIP through the shipped SparkBuild reader and check every name.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzListZipMembers(const std::uint8_t* data, std::size_t size);
