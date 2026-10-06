/**
 * @file FuzzTFLanBeaconProduction.h
 * @brief C ABI adapter for the TERRAFRONT LAN discovery beacon decoder and server-list merge
 *        (Terrafront::DecodeLanBeacon, Terrafront::UpsertLanServer).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeTFLanBeacon(const std::uint8_t* data, std::size_t size);
