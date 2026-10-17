/**
 * @file FuzzLocalizationProduction.h
 * @brief C ABI for the production localization catalog fuzz adapter.
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzLoadLocalization(const std::uint8_t* data, std::size_t size);
