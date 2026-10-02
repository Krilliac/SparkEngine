/**
 * @file FuzzEditorProjectFileProduction.h
 * @brief C ABI adapter for the editor project document and recent-projects readers
 *        (SparkEditor::ReadProjectDocumentFields, SparkEditor::ReadRecentProjectsDocument).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Read @p data as a project document and as a recent-projects list and check both.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzReadEditorProjectFile(const std::uint8_t* data, std::size_t size);
