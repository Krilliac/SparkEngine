/**
 * @file FuzzMMOClientStateProduction.h
 * @brief C ABI adapter for the SparkGameMMO client state request decoder
 *        (MMO::DecodeClientStateRequest).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeMMOClientState(const std::uint8_t* data, std::size_t size);
