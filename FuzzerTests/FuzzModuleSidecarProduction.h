/**
 * @file FuzzModuleSidecarProduction.h
 * @brief C ABI adapter for the .sparkabi sidecar gate (Spark::ModuleSidecar::ValidateModuleSidecar).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Validate @p data as a module's .sparkabi sidecar through the shipped gate and check the verdict.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzValidateModuleSidecar(const std::uint8_t* data, std::size_t size);
