#pragma once

#include <cstddef>
#include <cstdint>

extern "C" int SparkFuzzParseJson(const uint8_t* data, size_t size, uint32_t maxDepth);
