/**
 * @file BenchmarkFramework.h
 * @brief Performance regression benchmark framework with baseline comparison
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    struct BenchmarkMetric
    {
        std::string name;
        double value = 0.0;
        std::string unit;
        bool lowerIsBetter = true;
    };

    struct BenchmarkResult
    {
        std::string scenarioName;
        std::vector<BenchmarkMetric> metrics;
        uint32_t iterations = 0;
        std::string timestamp;
    };

    struct BenchmarkBaseline
    {
        std::string scenarioName;
        std::map<std::string, double> metrics;
        float tolerancePercent = 5.0f;
    };

    struct RegressionDetail
    {
        std::string metricName;
        double baseline = 0.0;
        double measured = 0.0;
        double percentChange = 0.0;
        double threshold = 0.0;
    };

    struct BenchmarkComparison
    {
        std::string scenarioName;
        bool passed = true;
        std::vector<RegressionDetail> regressions;
    };

    class IBenchmarkScenario
    {
      public:
        virtual ~IBenchmarkScenario() = default;
        [[nodiscard]] virtual std::string_view GetName() const = 0;
        virtual void Setup() {}
        [[nodiscard]] virtual std::vector<BenchmarkMetric> Run() = 0;
        virtual void TearDown() {}
        [[nodiscard]] virtual uint32_t GetIterationCount() const { return 10; }
    };

    class BenchmarkFramework
    {
      public:
        static BenchmarkFramework& GetInstance();

        void Initialize();
        void Shutdown();
        void RegisterScenario(std::unique_ptr<IBenchmarkScenario> scenario);

        /// Register the built-in dependency-free CPU/memory throughput canaries.
        /// Called from engine startup (not Initialize) so a running engine has
        /// real regression signal while unit tests keep an empty framework.
        void RegisterBuiltinScenarios();

        [[nodiscard]] std::vector<BenchmarkResult> RunAll();
        [[nodiscard]] BenchmarkResult RunScenario(std::string_view name);

        void SaveBaseline(std::string_view path, const std::vector<BenchmarkResult>& results) const;
        [[nodiscard]] std::vector<BenchmarkBaseline> LoadBaseline(std::string_view path) const;

        [[nodiscard]] std::vector<BenchmarkComparison> CompareWithBaseline(
            const std::vector<BenchmarkResult>& results, const std::vector<BenchmarkBaseline>& baselines) const;
        [[nodiscard]] bool HasRegressions(const std::vector<BenchmarkComparison>& comparisons) const;
        [[nodiscard]] std::string Console_GetStatus() const;

        /// Register the benchmark.* console commands (status/run). Called
        /// once during engine startup alongside the sibling diagnostic singletons
        /// so the framework is reachable from the console.
        void RegisterConsoleCommands();

      private:
        BenchmarkFramework() = default;
        BenchmarkFramework(const BenchmarkFramework&) = delete;
        BenchmarkFramework& operator=(const BenchmarkFramework&) = delete;

        struct AccumulatedMetric
        {
            double totalValue = 0.0;
            std::string unit;
            bool lowerIsBetter = true;
            uint32_t count = 0;
        };

        [[nodiscard]] IBenchmarkScenario* FindScenario(std::string_view name) const;
        [[nodiscard]] static std::string GetTimestamp();
        [[nodiscard]] static std::string EscapeJson(const std::string& str);
        [[nodiscard]] static std::string ExtractJsonString(const std::string& line);
        [[nodiscard]] static std::string Trim(const std::string& str);
        static void WriteFile(std::string_view path, const std::string& content);
        [[nodiscard]] static std::string ReadFile(std::string_view path);

      private:
        std::vector<std::unique_ptr<IBenchmarkScenario>> m_scenarios;
        bool m_initialized = false;
    };

} // namespace Spark
