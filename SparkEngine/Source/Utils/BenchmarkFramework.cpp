#include "BenchmarkFramework.h"

#include "Utils/SparkConsole.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>
#include <sstream>
#include <vector>

namespace Spark
{

    namespace
    {
        // Built-in, dependency-free, DETERMINISTIC throughput canaries. They give
        // RunAll() real regression signal out of the box (e.g. a compiler/flag
        // change that regresses basic CPU or allocator throughput shows up here);
        // feature code registers domain-specific scenarios via RegisterScenario().
        // Each Run() does one unit of work and returns its elapsed time; the
        // framework averages across GetIterationCount() runs.

        class CpuSortBenchmark final : public IBenchmarkScenario
        {
          public:
            [[nodiscard]] std::string_view GetName() const override { return "cpu.sort_1m"; }
            [[nodiscard]] uint32_t GetIterationCount() const override { return 5; }

            [[nodiscard]] std::vector<BenchmarkMetric> Run() override
            {
                constexpr size_t kCount = 1'000'000;
                std::vector<uint32_t> data(kCount);
                // Fixed LCG fill so the input (and therefore the timing) is reproducible.
                uint32_t state = 0x1234567u;
                for (auto& value : data)
                {
                    state = state * 1664525u + 1013904223u;
                    value = state;
                }

                const auto start = std::chrono::steady_clock::now();
                std::sort(data.begin(), data.end());
                const auto finish = std::chrono::steady_clock::now();

                const double ms = std::chrono::duration<double, std::milli>(finish - start).count();
                return {{"SortTime", ms, "ms", true}};
            }
        };

        class MemoryChurnBenchmark final : public IBenchmarkScenario
        {
          public:
            [[nodiscard]] std::string_view GetName() const override { return "mem.churn_4k_x10k"; }
            [[nodiscard]] uint32_t GetIterationCount() const override { return 5; }

            [[nodiscard]] std::vector<BenchmarkMetric> Run() override
            {
                constexpr size_t kBlocks = 10'000;
                constexpr size_t kBlockSize = 4096;

                const auto start = std::chrono::steady_clock::now();
                volatile uint8_t sink = 0;
                for (size_t i = 0; i < kBlocks; ++i)
                {
                    auto buffer = std::make_unique<uint8_t[]>(kBlockSize);
                    buffer[i % kBlockSize] = static_cast<uint8_t>(i);
                    sink = buffer[i % kBlockSize]; // defeat dead-store elimination
                }
                (void)sink;
                const auto finish = std::chrono::steady_clock::now();

                const double ms = std::chrono::duration<double, std::milli>(finish - start).count();
                return {{"ChurnTime", ms, "ms", true}};
            }
        };
    } // namespace

    BenchmarkFramework& BenchmarkFramework::GetInstance()
    {
        static BenchmarkFramework instance;
        return instance;
    }

    void BenchmarkFramework::Initialize()
    {
        // Starts empty by design: scenarios are registered by whoever wants to
        // benchmark (RegisterScenario / RegisterBuiltinScenarios). Keeping
        // Initialize() scenario-free preserves the framework contract the unit
        // tests pin (an initialized framework has zero scenarios until you add
        // some).
        m_scenarios.clear();
        m_initialized = true;
    }

    void BenchmarkFramework::RegisterBuiltinScenarios()
    {
        // The dependency-free CPU/memory throughput canaries. Registered from
        // engine startup (not Initialize) so `benchmark.run` has real regression
        // signal in a running engine, while unit tests keep a clean empty
        // framework to register their own stub scenarios against.
        RegisterScenario(std::make_unique<CpuSortBenchmark>());
        RegisterScenario(std::make_unique<MemoryChurnBenchmark>());
    }

    void BenchmarkFramework::Shutdown()
    {
        m_scenarios.clear();
        m_initialized = false;
    }

    void BenchmarkFramework::RegisterScenario(std::unique_ptr<IBenchmarkScenario> scenario)
    {
        if (scenario)
        {
            m_scenarios.push_back(std::move(scenario));
        }
    }

    std::vector<BenchmarkResult> BenchmarkFramework::RunAll()
    {
        std::vector<BenchmarkResult> results;
        results.reserve(m_scenarios.size());
        for (const auto& scenario : m_scenarios)
        {
            results.push_back(RunScenario(scenario->GetName()));
        }
        return results;
    }

    BenchmarkResult BenchmarkFramework::RunScenario(std::string_view name)
    {
        BenchmarkResult result;
        result.scenarioName = std::string(name);
        result.timestamp = GetTimestamp();

        auto* scenario = FindScenario(name);
        if (!scenario)
        {
            return result;
        }

        uint32_t iterations = scenario->GetIterationCount();
        result.iterations = iterations;

        scenario->Setup();

        std::map<std::string, AccumulatedMetric> accumulated;

        for (uint32_t i = 0; i < iterations; ++i)
        {
            auto metrics = scenario->Run();
            for (const auto& metric : metrics)
            {
                auto& acc = accumulated[metric.name];
                acc.totalValue += metric.value;
                acc.unit = metric.unit;
                acc.lowerIsBetter = metric.lowerIsBetter;
                acc.count++;
            }
        }

        scenario->TearDown();

        for (const auto& [metricName, acc] : accumulated)
        {
            BenchmarkMetric averaged;
            averaged.name = metricName;
            averaged.value = (acc.count > 0) ? acc.totalValue / acc.count : 0.0;
            averaged.unit = acc.unit;
            averaged.lowerIsBetter = acc.lowerIsBetter;
            result.metrics.push_back(std::move(averaged));
        }

        return result;
    }

    std::string BenchmarkFramework::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "[BenchmarkFramework] initialized=" << (m_initialized ? "true" : "false")
            << ", scenarios=" << m_scenarios.size();
        return oss.str();
    }

    void BenchmarkFramework::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "benchmark.status", [](const std::vector<std::string>&) -> std::string
            { return BenchmarkFramework::GetInstance().Console_GetStatus(); },
            "Show benchmark framework status and registered scenario count", "BenchmarkFramework");

        console.RegisterCommand(
            "benchmark.run",
            [](const std::vector<std::string>&) -> std::string
            {
                const std::vector<BenchmarkResult> results = BenchmarkFramework::GetInstance().RunAll();
                if (results.empty())
                    return "No benchmark scenarios registered.";

                std::ostringstream oss;
                oss << "Ran " << results.size() << " scenario(s):";
                for (const auto& result : results)
                {
                    oss << "\n  " << result.scenarioName << " (" << result.iterations << " iters)";
                    for (const auto& metric : result.metrics)
                        oss << std::format("  {}={:.3f}{}", metric.name, metric.value, metric.unit);
                }
                return oss.str();
            },
            "Run all registered benchmark scenarios and report averaged metrics", "BenchmarkFramework");
    }

    IBenchmarkScenario* BenchmarkFramework::FindScenario(std::string_view name) const
    {
        for (const auto& s : m_scenarios)
        {
            if (s->GetName() == name)
            {
                return s.get();
            }
        }
        return nullptr;
    }

    std::string BenchmarkFramework::GetTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&time));
        return std::string(buf);
    }

} // namespace Spark
