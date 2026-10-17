/**
 * @file FuzzEditorPrefabProduction.h
 * @brief C ABI adapter for the .sparkprefab production parser (SparkEditor::PrefabTextFormat::Parse).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseEditorPrefab(const std::uint8_t* data, std::size_t size);
