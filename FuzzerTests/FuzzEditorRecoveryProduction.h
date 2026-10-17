/**
 * @file FuzzEditorRecoveryProduction.h
 * @brief C ABI adapter for the editor recovery snapshot reader
 *        (SparkEditor::EditorRecoveryStore::LoadForProject).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Plant @p data as a project's recovery files and load them through the shipped store.
/// @param maxDepth The harness's envelope depth budget; checked against the shipped limit.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadEditorRecovery(const std::uint8_t* data, std::size_t size, std::uint32_t maxDepth);
