/**
 * @file FuzzEventResponseProduction.h
 * @brief C ABI adapter for the event-response rule file reader (Spark::Gameplay::ParseEventResponseRules).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as a rules file through the shipped reader and check the rules.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzParseEventResponseRules(const std::uint8_t* data, std::size_t size);
