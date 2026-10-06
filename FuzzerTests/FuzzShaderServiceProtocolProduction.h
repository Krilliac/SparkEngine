/**
 * @file FuzzShaderServiceProtocolProduction.h
 * @brief C ABI adapter for the shader daemon message decoders (ShaderServiceProtocol.h).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeShaderServiceMessage(const std::uint8_t* data, std::size_t size);
