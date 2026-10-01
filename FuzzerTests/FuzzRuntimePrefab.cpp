/**
 * @file FuzzRuntimePrefab.cpp
 * @brief Production-entry-point libFuzzer harness for the binary runtime prefab reader
 *        (Spark::ECS::RuntimePrefab::Deserialize).
 */

#include "FuzzRuntimePrefabProduction.h"

#include <cstddef>
#include <cstdint>

// A prefab holds components, and each component holds a flat property map.
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
    return SparkFuzzDeserializeRuntimePrefab(data, size);
}
