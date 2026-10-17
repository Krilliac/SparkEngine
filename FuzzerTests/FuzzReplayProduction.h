/** @brief C ABI adapter for the production ReplaySystem file loader. */
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzLoadReplay(const std::uint8_t* data, std::size_t size);
