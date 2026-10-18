/** @brief Production-entry-point libFuzzer harness for DataTable CSV/JSON loaders. */
#include "FuzzDataTableProduction.h"

#include <cstddef>
#include <cstdint>

constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 8388609;
constexpr std::uint32_t SPARK_FUZZ_MAX_DEPTH = 32;

static_assert(SPARK_FUZZ_MAX_DEPTH == 32u);

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > SPARK_FUZZ_MAX_INPUT_BYTES)
    {
        return 0;
    }
    if (data == nullptr && size != 0)
    {
        return 0;
    }
    return SparkFuzzParseDataTable(data, size);
}
