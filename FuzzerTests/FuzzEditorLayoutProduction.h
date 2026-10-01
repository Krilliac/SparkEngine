/**
 * @file FuzzEditorLayoutProduction.h
 * @brief C ABI adapter for the editor panel-layout reader (SparkEditor::EditorLayoutManager::LoadLayout).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Write @p data as a named layout and load it through the shipped layout manager.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadEditorLayout(const std::uint8_t* data, std::size_t size);
