/**
 * @file FuzzTelemetrySpool.cpp
 * @brief Production-entry-point libFuzzer harness for the telemetry spool
 *        decoder (Spark::TelemetryDetail::Parse).
 */

#include "FuzzTelemetrySpoolProduction.h"

#include <cstddef>
#include <cstdint>

// A spool is a header followed by a flat run of events, each a fixed prefix plus
// length-prefixed strings and one level of key/value properties.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 2;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == 2);

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
    return SparkFuzzParseTelemetrySpool(data, size);
}
