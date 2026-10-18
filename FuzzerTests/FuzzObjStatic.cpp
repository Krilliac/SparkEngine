/**
 * @file FuzzObjStatic.cpp
 * @brief Production-entry-point libFuzzer harness for the shared Wavefront OBJ
 *        static-mesh loader (Spark::Graphics::Detail::LoadOBJStaticMesh).
 */

#include "FuzzObjStaticProduction.h"

#include <cstddef>
#include <cstdint>

// OBJ is a flat list of statements; `mtllib` is the only reference to another
// file and the loader does not follow references from inside a material library.
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
    return SparkFuzzLoadObjStatic(data, size);
}
