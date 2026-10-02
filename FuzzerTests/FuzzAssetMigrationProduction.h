/**
 * @file FuzzAssetMigrationProduction.h
 * @brief C ABI adapter for the versioned asset migration reader (AssetMigrationRegistry::MigrateAsset).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Migrate @p data (expected asset type byte, then a SPRK file) through the shipped
///        registry and check the result against a model of the format.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzMigrateAsset(const std::uint8_t* data, std::size_t size);
