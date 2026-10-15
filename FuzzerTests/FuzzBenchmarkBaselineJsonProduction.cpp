/**
 * @file FuzzBenchmarkBaselineJsonProduction.cpp
 * @brief libc++-compiled production adapter for the benchmark baseline libFuzzer harness.
 *
 * BenchmarkFramework::LoadBaseline takes a path, so each input is written to a
 * private file under a per-process temporary directory and loaded from there.
 * The adapter compiles BenchmarkFrameworkBaseline.cpp, the production unit that
 * holds LoadBaseline, SaveBaseline, CompareWithBaseline and the line helpers.
 *
 * BenchmarkFramework's constructor is private and its only factory,
 * GetInstance(), lives in BenchmarkFramework.cpp, which pulls in the console
 * and the built-in scenarios and is not part of the parser. That translation
 * unit is not linked here; the adapter defines GetInstance() itself as the
 * test seam. It is a single-instance factory over the same private constructor
 * and touches no parser code.
 *
 * Each input is loaded, then the loaded baselines are compared against fixed
 * results so the consumer of the parsed values (CompareWithBaseline,
 * HasRegressions) also runs on attacker-chosen numbers. A one-time
 * Save/Load round trip of fixed results guards the adapter wiring itself.
 * A violated invariant aborts so libFuzzer records a crash.
 *
 * Thread affinity: libFuzzer calls this on one thread. The temporary directory
 * lives for the process and is removed at exit.
 */

#include "FuzzBenchmarkBaselineJsonProduction.h"

#include "Utils/BenchmarkFramework.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace Spark
{
    BenchmarkFramework& BenchmarkFramework::GetInstance()
    {
        static BenchmarkFramework instance;
        return instance;
    }
} // namespace Spark

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzBenchmarkBaselineJson infrastructure failure during %s: %s\n", operation,
                     std::strerror(errno));
        std::_Exit(70);
    }

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzBenchmarkBaselineJson: BenchmarkFramework violated: %s\n", what);
        std::abort();
    }

    /// The per-process directory described in the file comment.
    struct FuzzDirectory
    {
        std::filesystem::path base;
        std::filesystem::path input;
        std::filesystem::path roundTrip;

        FuzzDirectory()
        {
            // Honor TMPDIR like mkstemp-based tools do; fall back to /tmp.
            const char* tmpRoot = std::getenv("TMPDIR");
            std::string pattern = (tmpRoot != nullptr && tmpRoot[0] != '\0') ? tmpRoot : "/tmp";
            pattern += "/spark-baseline-fuzz-XXXXXX";
            if (::mkdtemp(pattern.data()) == nullptr)
                InfrastructureFailure("mkdtemp");
            base = pattern;
            input = base / "baseline.json";
            roundTrip = base / "round-trip.json";
        }

        ~FuzzDirectory()
        {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }
    };

    void WriteInput(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (size != 0)
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        out.close();
        if (!out)
            InfrastructureFailure("write");
    }

    std::vector<Spark::BenchmarkResult> FixedResults()
    {
        Spark::BenchmarkResult alpha;
        alpha.scenarioName = "alpha";
        alpha.metrics = {{"FrameTime", 12.5, "ms", true}, {"Throughput", 480.0, "ops", false}};
        Spark::BenchmarkResult beta;
        beta.scenarioName = "beta";
        beta.metrics = {{"SortTime", 3.25, "ms", true}};
        return {alpha, beta};
    }

    /// Save then load fixed results: proves the adapter reaches the real reader and writer.
    void CheckRoundTrip(const FuzzDirectory& directory)
    {
        Spark::BenchmarkFramework& framework = Spark::BenchmarkFramework::GetInstance();
        const std::vector<Spark::BenchmarkResult> results = FixedResults();
        framework.SaveBaseline(directory.roundTrip.string(), results);
        const std::vector<Spark::BenchmarkBaseline> loaded = framework.LoadBaseline(directory.roundTrip.string());
        if (loaded.size() != results.size())
            InvariantFailure("a saved baseline does not load back with the same scenario count");
        for (std::size_t i = 0; i < loaded.size(); ++i)
        {
            if (loaded[i].scenarioName != results[i].scenarioName ||
                loaded[i].metrics.size() != results[i].metrics.size())
                InvariantFailure("a saved baseline does not load back with the same scenarios and metrics");
            for (const Spark::BenchmarkMetric& metric : results[i].metrics)
            {
                const auto found = loaded[i].metrics.find(metric.name);
                if (found == loaded[i].metrics.end() || found->second != metric.value)
                    InvariantFailure("a saved metric does not load back with the same value");
            }
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production reader.
extern "C" int SparkFuzzLoadBenchmarkBaseline(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    static const FuzzDirectory directory;
    static const bool roundTripChecked = (CheckRoundTrip(directory), true);
    (void)roundTripChecked;

    WriteInput(directory.input, data, size);
    Spark::BenchmarkFramework& framework = Spark::BenchmarkFramework::GetInstance();
    const std::vector<Spark::BenchmarkBaseline> baselines = framework.LoadBaseline(directory.input.string());

    // An empty file loads as no baselines (ReadFile reports it as empty).
    if (size == 0 && !baselines.empty())
        InvariantFailure("an empty file produced baselines");
    for (const Spark::BenchmarkBaseline& baseline : baselines)
    {
        if (baseline.scenarioName.empty())
            InvariantFailure("an accepted baseline has no scenario name");
    }

    // Every scenario in the fixed result set must be answered, whatever the file held.
    const std::vector<Spark::BenchmarkResult> results = FixedResults();
    const std::vector<Spark::BenchmarkComparison> comparisons = framework.CompareWithBaseline(results, baselines);
    if (comparisons.size() != results.size())
        InvariantFailure("CompareWithBaseline did not return one comparison per result");
    (void)framework.HasRegressions(comparisons);
    return 0;
}
