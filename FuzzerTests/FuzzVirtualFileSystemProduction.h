/**
 * @file FuzzVirtualFileSystemProduction.h
 * @brief C ABI adapter for the virtual-filesystem mount resolver (Spark::VirtualFileSystem::ReadFile).
 */

#pragma once

#include <cstddef>
#include <cstdint>

/// @brief Resolve @p data as a mod-supplied virtual path through the shipped VFS and check the result.
/// @return 0 (libFuzzer convention); an invariant violation aborts the process.
extern "C" int SparkFuzzVirtualFileSystemRead(const std::uint8_t* data, std::size_t size);
