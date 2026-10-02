/**
 * @file FuzzVisualScriptGraphProduction.h
 * @brief C ABI adapter for the .vscript graph decoder (VisualScriptGraphIO::Parse).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Parse @p data as a .vscript file through the shipped decoder and check the canonical round trip.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzParseVisualScriptGraph(const std::uint8_t* data, std::size_t size);
