/**
 * @file FuzzEditorSceneImportProduction.h
 * @brief C ABI adapter for the Scene Import panel's game INI reader (SparkEditor::ParseGameSceneIni).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as a game INI .scene through the shipped reader and check the document.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzParseEditorSceneImport(const std::uint8_t* data, std::size_t size);
