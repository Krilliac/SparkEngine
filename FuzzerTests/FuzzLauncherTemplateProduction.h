/**
 * @file FuzzLauncherTemplateProduction.h
 * @brief C ABI adapter for the launcher template.json reader (SparkLauncher::ReadTemplateEntry).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Plant @p data as a template's template.json and read it through the shipped reader.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzReadLauncherTemplate(const std::uint8_t* data, std::size_t size);
