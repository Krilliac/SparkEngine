/**
 * @file FuzzSaveSystemProduction.h
 * @brief C ABI adapter for the .spark_save production decoder (Spark::DecodeSaveFileBytes).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeSaveFile(const std::uint8_t* data, std::size_t size);
