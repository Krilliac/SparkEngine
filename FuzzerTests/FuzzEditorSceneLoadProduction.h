/**
 * @file FuzzEditorSceneLoadProduction.h
 * @brief C ABI adapter for the editor scene load entry (SparkEditor::SceneSerializer::LoadScene).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Write @p data as a scene file and load it through the shipped SceneSerializer.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadEditorScene(const std::uint8_t* data, std::size_t size);
