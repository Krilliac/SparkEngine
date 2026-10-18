/**
 * @file FuzzGatewayAreaControlStateProduction.h
 * @brief C ABI adapter for the gateway area-control epoch state reader
 *        (Spark::Gateway::ParseAreaControlState).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseGatewayAreaControlState(const std::uint8_t* data, std::size_t size);
