/**
 * @file FuzzAssetServiceProtocolProduction.h
 * @brief C ABI adapter for the asset daemon message decoders (AssetServiceProtocol.h).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeAssetServiceMessage(const std::uint8_t* data, std::size_t size);
