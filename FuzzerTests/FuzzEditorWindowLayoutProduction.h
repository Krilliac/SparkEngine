/**
 * @file FuzzEditorWindowLayoutProduction.h
 * @brief C ABI adapter for the editor window-layout reader
 *        (SparkEditor::EditorWindowManager::LoadLayoutFromFile).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Write @p data as the window-layout file and load it through the shipped manager.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadEditorWindowLayout(const std::uint8_t* data, std::size_t size);
