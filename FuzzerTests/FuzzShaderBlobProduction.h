/**
 * @file FuzzShaderBlobProduction.h
 * @brief C ABI adapter for the shader-daemon blob decoder (DecodeCompiledShaderBlob).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeShaderBlob(const std::uint8_t* data, std::size_t size);
