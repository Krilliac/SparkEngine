/**
 * @file FuzzSparkTerrainProduction.h
 * @brief C ABI adapter for the runtime .sparkterrain decoder (SparkTerrain::DecodeRuntime).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeSparkTerrain(const std::uint8_t* data, std::size_t size);
