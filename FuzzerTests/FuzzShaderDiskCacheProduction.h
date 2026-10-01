/**
 * @file FuzzShaderDiskCacheProduction.h
 * @brief C ABI adapter for the shader disk-cache blob reader (Spark::Graphics::ShaderDiskCache::Lookup).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Plant @p data as a cached shader blob and read it back through the shipped cache.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLookupShaderDiskCache(const std::uint8_t* data, std::size_t size);
