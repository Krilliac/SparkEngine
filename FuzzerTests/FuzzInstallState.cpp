/**
 * @file FuzzInstallState.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkInstaller install-tree
 *        markers (SparkInstaller::InstallState::Load and ReadPendingMarker).
 */

#include "FuzzInstallStateProduction.h"

#include <cstddef>
#include <cstdint>

// The install-state object has one nested level (the "options" object inside the
// top-level object); the pending marker is flat key=value lines.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 2;
// One byte past InstallState's 64 KiB file cap, so the oversize reject is reachable.
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 65537;

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
    return SparkFuzzLoadInstallState(data, size);
}
