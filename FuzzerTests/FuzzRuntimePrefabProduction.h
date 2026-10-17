/**
 * @file FuzzRuntimePrefabProduction.h
 * @brief C ABI adapter for the binary runtime prefab reader (Spark::ECS::RuntimePrefab::Deserialize).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Decode @p data as a PRFB prefab through the shipped reader and check the result.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzDeserializeRuntimePrefab(const std::uint8_t* data, std::size_t size);
