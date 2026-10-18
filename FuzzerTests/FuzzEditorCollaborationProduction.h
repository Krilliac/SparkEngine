/**
 * @file FuzzEditorCollaborationProduction.h
 * @brief C ABI adapter for SparkEditor collaboration session wire decoding.
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeEditorCollaboration(const std::uint8_t* data, std::size_t size);
