/**
 * @file FuzzExrProduction.h
 * @brief C ABI adapter for the bounded OpenEXR production loader (EXRLoader::Load).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzLoadExr(const std::uint8_t* data, std::size_t size);
