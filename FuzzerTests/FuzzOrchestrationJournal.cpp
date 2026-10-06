/**
 * @file FuzzOrchestrationJournal.cpp
 * @brief Production-entry-point libFuzzer harness for the SparkDaemon orchestration
 *        journal snapshot and write-ahead log (Spark::Daemon::RecoverOrchestrationJournal).
 */

#include "FuzzOrchestrationJournalProduction.h"

#include <cstddef>
#include <cstdint>

// The snapshot nests one level of length-prefixed blobs (process definition and
// status) inside each process record; WAL records are flat.
constexpr uint32_t SPARK_FUZZ_MAX_DEPTH = 2;
constexpr std::size_t SPARK_FUZZ_MAX_INPUT_BYTES = 262144;

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
    return SparkFuzzRecoverOrchestrationJournal(data, size);
}
