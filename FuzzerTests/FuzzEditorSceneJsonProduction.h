/**
 * @file FuzzEditorSceneJsonProduction.h
 * @brief C ABI adapter for the editor JSON scene decoder (SparkEditor::DecodeSceneJSONDocument).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Decode @p data as an editor JSON scene through the shipped decoder and check it.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzDecodeEditorSceneJson(const std::uint8_t* data, std::size_t size);
