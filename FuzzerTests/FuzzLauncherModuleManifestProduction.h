/**
 * @file FuzzLauncherModuleManifestProduction.h
 * @brief C ABI adapter for the launcher spark.modules.json reader (SparkLauncher::BuildLaunchRequest).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Plant @p data as a project's spark.modules.json and build a Game launch request for it.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzResolveLauncherManifest(const std::uint8_t* data, std::size_t size);
