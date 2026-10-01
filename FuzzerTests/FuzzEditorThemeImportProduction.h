/**
 * @file FuzzEditorThemeImportProduction.h
 * @brief C ABI adapter for the editor theme import reader (SparkEditor::ParseThemeDocument).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as an exported theme through the shipped reader and check the theme.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzParseEditorTheme(const std::uint8_t* data, std::size_t size);
