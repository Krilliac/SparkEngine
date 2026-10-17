/** @brief C ABI adapter for the production DataTable CSV/JSON loaders. */
#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseDataTable(const std::uint8_t* data, std::size_t size);
