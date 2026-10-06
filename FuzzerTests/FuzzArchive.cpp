/**
 * @file FuzzArchive.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkPak (.spk)
 *        archive reader (Spark::SparkPakReader::Open and ReadFile).
 */

#include "FuzzArchiveProduction.h"

#include <cstddef>
#include <cstdint>

// A SparkPak archive is a header, flat data blobs and a flat table of contents;
// entries never contain other entries, so the depth budget is a single level.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

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
    return SparkFuzzOpenSparkPak(data, size);
}
