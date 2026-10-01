/**
 * @file FuzzUILayoutProduction.h
 * @brief C ABI adapter for the UI layout readers (UILayoutLoader::LoadFromJSON, UIFactory::ParseConfig).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Load @p data as a UI layout and as a widget config through the shipped readers and
///        check the widget trees they build.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadUILayout(const std::uint8_t* data, std::size_t size);
