/**
 * @file FuzzDaemonWireProduction.h
 * @brief C ABI adapter for the daemon collaboration and orchestration DTO decoders
 *        (SparkDaemon/src CollaborationProtocol.h, OrchestrationProtocol.h over BoundedWireCodec.h).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeDaemonWireMessage(const std::uint8_t* data, std::size_t size);
