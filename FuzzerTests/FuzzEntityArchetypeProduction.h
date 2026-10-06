/**
 * @file FuzzEntityArchetypeProduction.h
 * @brief C ABI adapter for the .archetype reader (Spark::ECS::ParseArchetypeDefinition).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as an .archetype file through the shipped reader and check the result.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzParseArchetype(const std::uint8_t* data, std::size_t size);
