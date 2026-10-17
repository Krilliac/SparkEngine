/**
 * @file FuzzMMOEntityEventsProduction.h
 * @brief C ABI adapter for the SparkGameMMO client-side EntitySpawn / EntityDestroy decoders
 *        (MMO::DecodeEntitySpawn, MMO::DecodeEntityDestroy).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeMMOEntityEvents(const std::uint8_t* data, std::size_t size);
