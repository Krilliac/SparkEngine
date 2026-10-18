/**
 * @file FuzzFbxProduction.h
 * @brief C ABI adapter for the binary FBX production importer (FBXImporter::ImportFromMemory).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Import @p data as a binary FBX document through the shipped importer with every option on.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzImportFbx(const std::uint8_t* data, std::size_t size);
