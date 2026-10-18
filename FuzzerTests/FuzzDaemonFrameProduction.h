/**
 * @file FuzzDaemonFrameProduction.h
 * @brief C ABI adapter for the daemon IPC frame receive path (DaemonFraming.h RecvFrame).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzRecvDaemonFrames(const std::uint8_t* data, std::size_t size);
