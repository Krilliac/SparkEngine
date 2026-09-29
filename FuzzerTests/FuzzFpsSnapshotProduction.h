/**
 * @file FuzzFpsSnapshotProduction.h
 * @brief C ABI for the production FPS snapshot-batch fuzz adapter.
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzProcessFpsSnapshot(const std::uint8_t* data, std::size_t size);
