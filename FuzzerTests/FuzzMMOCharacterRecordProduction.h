/**
 * @file FuzzMMOCharacterRecordProduction.h
 * @brief C ABI adapter for the SparkGameMMO character row codec (MMO::DecodeCharacterRecord).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeMMOCharacterRecord(const std::uint8_t* data, std::size_t size);
