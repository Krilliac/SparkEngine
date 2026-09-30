/**
 * @file FuzzAsyncDatabase.cpp
 * @brief Production-entry-point libFuzzer harness for the AsyncDatabase key/value
 *        store file (Spark::Persistence::SQLiteConnection::Open).
 */

#include "FuzzAsyncDatabaseProduction.h"

#include <cstddef>
#include <cstdint>

// The store is flat "key<TAB>value" lines; nothing nests.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
// One byte past the adapter's 64 KiB store budget, so the oversize reject is reachable.
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65537;

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
    return SparkFuzzOpenAsyncDatabase(data, size);
}
