/** @brief C ABI adapter for the production .skel/.sanim binary decoders. */
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzDecodeAnimationBinary(const std::uint8_t* data, std::size_t size);
