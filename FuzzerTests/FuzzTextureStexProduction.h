/**
 * @file FuzzTextureStexProduction.h
 * @brief C ABI adapter for the .stex production loader (TextureCompressor::LoadCompressed).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzLoadStex(const std::uint8_t* data, std::size_t size);
