/**
 * @file FuzzTFLanBeacon.cpp
 * @brief Production-entry-point libFuzzer harness for the TERRAFRONT LAN discovery beacon decoder
 *        and server-list merge (Terrafront::DecodeLanBeacon, Terrafront::UpsertLanServer).
 */

#include "FuzzTFLanBeaconProduction.h"

#include <cstddef>
#include <cstdint>

// A beacon is one fixed 68-byte record with two fixed-size string fields and no nesting;
// the input is a flat sequence of framed datagrams, large enough for 65 of them.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 8192;

static_assert(SPARK_FUZZ_MAX_DEPTH == 1);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }
    if (data == nullptr && size != 0)
    {
        return 0;
    }
    return SparkFuzzDecodeTFLanBeacon(data, size);
}
