/**
 * @file FuzzNavMesh.cpp
 * @brief Production-entry-point libFuzzer harness for the .snav navmesh decoder
 *        (Spark::AI::DecodeSnav, the decoder behind NavMeshManager::LoadNavMesh).
 */

#include "FuzzNavMeshProduction.h"

#include <cstddef>
#include <cstdint>

// A .snav is a header, a flat vertex array and a triangle array whose records each
// carry one nested run of adjacency entries.
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
    return SparkFuzzDecodeSnav(data, size);
}
