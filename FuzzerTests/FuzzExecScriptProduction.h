/**
 * @file FuzzExecScriptProduction.h
 * @brief C ABI adapter for the `-exec` console script parser (Spark::ParseExecScript).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as an `-exec` script through the shipped parser and check the schedule.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzParseExecScript(const std::uint8_t* data, std::size_t size);
