/**
 * @file FuzzBenchmarkBaselineJsonProduction.h
 * @brief C ABI adapter for the benchmark baseline reader (Spark::BenchmarkFramework::LoadBaseline).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Write @p data to a baseline file, load it through the shipped reader and compare against it.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzLoadBenchmarkBaseline(const std::uint8_t* data, std::size_t size);
