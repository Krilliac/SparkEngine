/**
 * @file FuzzMMOChatWireProduction.h
 * @brief C ABI adapter for the SparkGameMMO chat wire decoder and server relay policy
 *        (MMO::MMOChatSystem::DecodeWirePayload, MMO::MMOChatSystem::BuildServerRelayPayload).
 */

#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeMMOChat(const std::uint8_t* data, std::size_t size);
