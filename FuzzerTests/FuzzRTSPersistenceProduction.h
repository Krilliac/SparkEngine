/**
 * @file FuzzRTSPersistenceProduction.h
 * @brief C ABI adapter for the SparkGameRTS save snapshot decoder (RTS::RTSPersistence::Deserialize).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDeserializeRTSSnapshot(const std::uint8_t* data, std::size_t size);
