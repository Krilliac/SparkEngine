/**
 * @file FuzzInstallStateProduction.h
 * @brief C ABI adapter for the SparkInstaller install-tree marker readers
 *        (SparkInstaller::InstallState::Load and InstallState::ReadPendingMarker).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzLoadInstallState(const std::uint8_t* data, std::size_t size);
