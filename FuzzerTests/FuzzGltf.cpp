/**
 * @file FuzzGltf.cpp
 * @brief Production-entry-point libFuzzer harness for the glTF/GLB mesh loaders
 *        (Spark::Graphics::Detail::LoadGLTFStaticMesh, LoadGLTFSkinnedMesh and
 *        LoadGLTFAnimationClips over the shared GLTF::ParseDocument).
 */

#include "FuzzGltfProduction.h"

#include <cstddef>
#include <cstdint>

// The deepest structure the loaders accept is a single-chain skin: at most
// kMaxBonesPerMesh (256) joints, each the child of the previous one.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 256;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65536;

static_assert(SPARK_FUZZ_MAX_DEPTH == 256);

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
    return SparkFuzzLoadGltf(data, size);
}
