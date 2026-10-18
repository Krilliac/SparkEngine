/**
 * @file FuzzBinaryReaderProduction.h
 * @brief C ABI adapter for the shared binary reader (Utils/Serializer.h Spark::BinaryReader).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzRunBinaryReaderScript(const std::uint8_t* data, std::size_t size);
