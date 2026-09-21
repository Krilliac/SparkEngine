/**
 * @file FuzzCrashManifestProduction.h
 * @brief C ABI adapter for the crash-manifest production parser.
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseCrashManifest(const std::uint8_t* data, std::size_t size);
