/**
 * @file FuzzSparkBuildConfig.cpp
 * @brief Production-entry-point libFuzzer harness for SparkBuild's sparkbuild.ini
 *        reader (SparkBuild::ConfigManager::LoadFromStream) and the shell
 *        commands built from it.
 */

#include "FuzzSparkBuildConfigProduction.h"

#include <cstddef>
#include <cstdint>

// sparkbuild.ini is a flat list of section headers and key=value lines with
// no nesting, so the depth budget is a single level. The input bound matches
// ConfigManager::kMaxConfigBytes, the largest file Load reads.
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
    return SparkFuzzLoadSparkBuildConfig(data, size);
}
