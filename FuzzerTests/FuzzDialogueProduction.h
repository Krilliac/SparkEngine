/** @brief C ABI adapter for the production DialogueTree file loader. */
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseDialogue(const std::uint8_t* data, std::size_t size);
