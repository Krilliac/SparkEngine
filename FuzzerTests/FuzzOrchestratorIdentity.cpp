/**
 * @file FuzzOrchestratorIdentity.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkOrchestrator mutation
 *        identity state file (Spark::Daemon::OrchestratorIdentityLease::Acquire).
 */

#include "FuzzOrchestratorIdentityProduction.h"

#include <cstddef>
#include <cstdint>

// The state is a magic line, a client-instance line and a sequence line.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 1;
// One byte past the reader's 256-byte state cap, so the oversize reject is reachable.
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 257;

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
    return SparkFuzzAcquireOrchestratorIdentity(data, size);
}
