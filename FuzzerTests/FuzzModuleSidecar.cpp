/**
 * @file FuzzModuleSidecar.cpp
 * @brief Production-entry-point libFuzzer harness for the game-module .sparkabi sidecar gate
 *        (Spark::ModuleSidecar::ValidateModuleSidecar).
 */

#include "FuzzModuleSidecarProduction.h"

#include <cstddef>
#include <cstdint>

// A sidecar is a flat list of key=value lines. The input may exceed the reader's
// 4096-byte cap so the cap itself is exercised.
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
    return SparkFuzzValidateModuleSidecar(data, size);
}
