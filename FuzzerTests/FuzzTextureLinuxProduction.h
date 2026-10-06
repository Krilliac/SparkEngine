/**
 * @file FuzzTextureLinuxProduction.h
 * @brief C ABI adapter for the non-Windows texture file loader (Texture::CreateFromFile).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzCreateTextureFromFile(const std::uint8_t* data, std::size_t size);
