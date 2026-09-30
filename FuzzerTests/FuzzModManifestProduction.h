/**
 * @file FuzzModManifestProduction.h
 * @brief C ABI adapter for mod discovery (Spark::ModSystem::ScanForMods and LoadConfig).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzScanMods(const std::uint8_t* data, std::size_t size);
