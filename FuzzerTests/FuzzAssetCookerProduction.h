/**
 * @file FuzzAssetCookerProduction.h
 * @brief C ABI adapter for the asset cooker's source-tree reader (Spark::AssetPipeline::CookAssets).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Build the source tree @p data describes, cook it through the shipped cooker and check
///        the published output and manifest.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzCookAssetTree(const std::uint8_t* data, std::size_t size);
