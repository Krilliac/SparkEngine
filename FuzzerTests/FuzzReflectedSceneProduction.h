/**
 * @file FuzzReflectedSceneProduction.h
 * @brief C ABI adapter for the reflected JSON scene reader (Spark::DeserializeInto).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @p maxDepth is the harness's declared nesting budget; the adapter aborts when
/// it no longer matches the reader's own parse-depth limit.
extern "C" int SparkFuzzDeserializeReflectedScene(const std::uint8_t* data, std::size_t size, std::uint32_t maxDepth);
