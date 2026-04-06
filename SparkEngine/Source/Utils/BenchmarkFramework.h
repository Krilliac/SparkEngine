/**
 * @file BenchmarkFramework.h
 * @brief Performance regression benchmark framework with baseline comparison
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a structured way to define, run, and compare performance benchmarks
 * against saved baselines. Detects regressions by comparing measured metrics
 * against stored thresholds.
 *
 * ## Usage
 * @code
 *   class PhysicsBenchmark : public Spark::IBenchmarkScenario
 *   {
 *   public:
 *       std::string_view GetName() const override { return "Physics_1000Bodies"; }
 *       void Setup() override { }
 *       std::vector<Spark::BenchmarkMetric> Run() override
 *       {
 *           auto start = std::chrono::high_resolution_clock::now();
 *           // simulate physics for N frames
 *           double ms = 1.0; // elapsed time
 *           return {{"StepTime", ms, "ms", true}};
 *       }
 *       void TearDown() override { }
 *   };
 *
 *   auto& bench = Spark::BenchmarkFramework::GetInstance();
 *   bench.Initialize();
 *   bench.RegisterScenario(std::make_unique<PhysicsBenchmark>());
 *   auto results = bench.RunAll();
 *   bench.SaveBaseline("baselines.json", results);
 * @endcode
 */

#pragma once

#include <optional>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    // ============================================================================
    // Data types
    // ============================================================================

    /**
     * @brief A single named metric from a benchmark run.
     */
    struct BenchmarkMetric
    {
        std::string name;          ///< Metric name (e.g. "FrameTime")
        double value = 0.0;        ///< Measured value
        std::string unit;          ///< Unit string (e.g. "ms", "MB", "count")
        bool lowerIsBetter = true; ///< Whether lower values indicate better performance
    };

    /**
     * @brief Results from running a single benchmark scenario.
     */
    struct BenchmarkResult
    {
        std::string scenarioName;             ///< Name of the scenario that was run
        std::vector<BenchmarkMetric> metrics; ///< All collected metrics
        uint32_t iterations = 0;              ///< Number of iterations executed
        std::string timestamp;                ///< ISO-8601 timestamp of the run
    };

    /**
     * @brief Stored baseline values for a benchmark scenario.
     */
    struct BenchmarkBaseline
    {
        std::string scenarioName;              ///< Scenario this baseline belongs to
        std::map<std::string, double> metrics; ///< Metric name -> baseline value
        float tolerancePercent = 5.0f;         ///< Allowed deviation before flagging regression
    };

    /**
     * @brief Detail about a single metric that regressed.
     */
    struct RegressionDetail
    {
        std::string metricName;     ///< Which metric regressed
        double baseline = 0.0;      ///< Expected baseline value
        double measured = 0.0;      ///< Actually measured value
        double percentChange = 0.0; ///< Percentage change from baseline
        double threshold = 0.0;     ///< Allowed threshold percentage
    };

    /**
     * @brief Comparison result for a single scenario against its baseline.
     */
    struct BenchmarkComparison
    {
        std::string scenarioName;                  ///< Scenario that was compared
        bool passed = true;                        ///< True if no regressions detected
        std::vector<RegressionDetail> regressions; ///< Details of any regressions found
    };

    // ============================================================================
    // Benchmark scenario interface
    // ============================================================================

    /**
     * @brief Interface for defining a benchmark scenario.
     *
     * Implement this to create a repeatable performance test. The framework
     * calls Setup(), then Run() for GetIterationCount() iterations, then TearDown().
     */
    class IBenchmarkScenario
    {
      public:
        virtual ~IBenchmarkScenario() = default;

        /// @brief Unique name for this scenario
        [[nodiscard]] virtual std::string_view GetName() const = 0;

        /// @brief One-time setup before iterations begin
        virtual void Setup() {}

        /// @brief Execute a single iteration and return collected metrics
        /// @return Metrics measured during this iteration
        [[nodiscard]] virtual std::vector<BenchmarkMetric> Run() = 0;

        /// @brief One-time teardown after all iterations complete
        virtual void TearDown() {}

        /// @brief Number of iterations to run (default 10)
        [[nodiscard]] virtual uint32_t GetIterationCount() const { return 10; }
    };

    // ============================================================================
    // Benchmark framework
    // ============================================================================

    /**
     * @brief Singleton framework for running and comparing performance benchmarks.
     *
     * Register scenarios, run them, save/load baselines, and detect regressions.
     */
    class BenchmarkFramework
    {
      public:
        static BenchmarkFramework& GetInstance()
        {
            static BenchmarkFramework instance;
            return instance;
        }

        /// @brief Initialize the framework, clearing any registered scenarios
        void Initialize()
        {
            m_scenarios.clear();
            m_initialized = true;
        }

        /// @brief Shut down the framework and release all scenarios
        void Shutdown()
        {
            m_scenarios.clear();
            m_initialized = false;
        }

        /// @brief Register a benchmark scenario
        /// @param scenario Owned scenario to register
        void RegisterScenario(std::unique_ptr<IBenchmarkScenario> scenario)
        {
            if (scenario)
            {
                m_scenarios.push_back(std::move(scenario));
            }
        }

        /// @brief Run all registered scenarios and return their results
        /// @return Results for every registered scenario
        [[nodiscard]] std::vector<BenchmarkResult> RunAll()
        {
            std::vector<BenchmarkResult> results;
            results.reserve(m_scenarios.size());
            for (const auto& scenario : m_scenarios)
            {
                results.push_back(RunScenario(scenario->GetName()));
            }
            return results;
        }

        /// @brief Run a single scenario by name
        /// @param name Scenario name to find and run
        /// @return The benchmark result (empty if not found)
        [[nodiscard]] BenchmarkResult RunScenario(std::string_view name)
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

            // Accumulate metrics across iterations, then average
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

            // Produce averaged metrics
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

        // ====================================================================
        // Baseline serialization (simple JSON format)
        // ====================================================================

        /// @brief Save results as a baseline JSON file
        /// @param path File path to write
        /// @param results Results to serialize as baselines
        void SaveBaseline(std::string_view path, const std::vector<BenchmarkResult>& results) const
        {
            std::ostringstream json;
            json << "{\n  \"baselines\": [\n";

            for (size_t i = 0; i < results.size(); ++i)
            {
                const auto& result = results[i];
                json << "    {\n";
                json << "      \"scenario\": \"" << EscapeJson(result.scenarioName) << "\",\n";
                json << "      \"metrics\": {\n";

                for (size_t j = 0; j < result.metrics.size(); ++j)
                {
                    const auto& metric = result.metrics[j];
                    json << "        \"" << EscapeJson(metric.name) << "\": " << metric.value;
                    if (j + 1 < result.metrics.size())
                        json << ",";
                    json << "\n";
                }

                json << "      },\n";
                json << "      \"tolerance\": 5.0\n";
                json << "    }";
                if (i + 1 < results.size())
                    json << ",";
                json << "\n";
            }

            json << "  ]\n}\n";

            WriteFile(path, json.str());
        }

        /// @brief Load baselines from a JSON file
        /// @param path File path to read
        /// @return Parsed baselines (empty on failure)
        [[nodiscard]] std::vector<BenchmarkBaseline> LoadBaseline(std::string_view path) const
        {
            std::vector<BenchmarkBaseline> baselines;
            std::string content = ReadFile(path);
            if (content.empty())
            {
                return baselines;
            }

            // Simple line-by-line JSON parser for our known format
            std::istringstream stream(content);
            std::string line;
            BenchmarkBaseline current;
            bool inBaseline = false;
            bool inMetrics = false;

            while (std::getline(stream, line))
            {
                std::string trimmed = Trim(line);

                if (trimmed.find("\"scenario\"") != std::string::npos)
                {
                    current = BenchmarkBaseline{};
                    inBaseline = true;
                    current.scenarioName = ExtractJsonString(trimmed);
                }
                else if (inBaseline && trimmed == "\"metrics\": {")
                {
                    inMetrics = true;
                }
                else if (inMetrics && trimmed == "}" && !trimmed.empty())
                {
                    // Could be end of metrics block — simple heuristic
                    if (trimmed == "}," || trimmed == "}")
                    {
                        inMetrics = false;
                    }
                }
                else if (inMetrics)
                {
                    // Parse "name": value lines
                    auto colonPos = trimmed.find(':');
                    if (colonPos != std::string::npos && trimmed.front() == '"')
                    {
                        std::string metricName = trimmed.substr(1, trimmed.find('"', 1) - 1);
                        std::string valueStr = Trim(trimmed.substr(colonPos + 1));
                        // Remove trailing comma
                        if (!valueStr.empty() && valueStr.back() == ',')
                        {
                            valueStr.pop_back();
                        }
                        try
                        {
                            double val = std::stod(valueStr);
                            current.metrics[metricName] = val;
                        }
                        catch (...)
                        {
                            // Skip malformed values
                        }
                    }
                }
                else if (trimmed.find("\"tolerance\"") != std::string::npos)
                {
                    auto colonPos = trimmed.find(':');
                    if (colonPos != std::string::npos)
                    {
                        std::string valueStr = Trim(trimmed.substr(colonPos + 1));
                        try
                        {
                            current.tolerancePercent = std::stof(valueStr);
                        }
                        catch (...)
                        {
                        }
                    }
                }
                else if (inBaseline && (trimmed == "}," || trimmed == "}"))
                {
                    if (!current.scenarioName.empty())
                    {
                        baselines.push_back(std::move(current));
                        current = BenchmarkBaseline{};
                        inBaseline = false;
                    }
                }
            }

            return baselines;
        }

        // ====================================================================
        // Comparison
        // ====================================================================

        /// @brief Compare results against baselines to detect regressions
        /// @param results Current benchmark results
        /// @param baselines Previously saved baselines
        /// @return Comparison details for each matched scenario
        [[nodiscard]] std::vector<BenchmarkComparison> CompareWithBaseline(
            const std::vector<BenchmarkResult>& results, const std::vector<BenchmarkBaseline>& baselines) const
        {
            std::vector<BenchmarkComparison> comparisons;

            for (const auto& result : results)
            {
                // Find matching baseline
                const BenchmarkBaseline* baseline = nullptr;
                for (const auto& bl : baselines)
                {
                    if (bl.scenarioName == result.scenarioName)
                    {
                        baseline = &bl;
                        break;
                    }
                }

                if (!baseline)
                {
                    continue;
                }

                BenchmarkComparison comparison;
                comparison.scenarioName = result.scenarioName;
                comparison.passed = true;

                for (const auto& metric : result.metrics)
                {
                    auto it = baseline->metrics.find(metric.name);
                    if (it == baseline->metrics.end())
                    {
                        continue;
                    }

                    double baselineValue = it->second;
                    if (baselineValue == 0.0)
                    {
                        continue;
                    }

                    double percentChange = ((metric.value - baselineValue) / baselineValue) * 100.0;

                    // For "lower is better" metrics, a positive change is a regression.
                    // For "higher is better" metrics, a negative change is a regression.
                    bool isRegression = metric.lowerIsBetter ? (percentChange > baseline->tolerancePercent)
                                                             : (percentChange < -baseline->tolerancePercent);

                    if (isRegression)
                    {
                        RegressionDetail detail;
                        detail.metricName = metric.name;
                        detail.baseline = baselineValue;
                        detail.measured = metric.value;
                        detail.percentChange = percentChange;
                        detail.threshold = baseline->tolerancePercent;
                        comparison.regressions.push_back(std::move(detail));
                        comparison.passed = false;
                    }
                }

                comparisons.push_back(std::move(comparison));
            }

            return comparisons;
        }

        /// @brief Check if any comparisons contain regressions
        /// @param comparisons Results of CompareWithBaseline
        /// @return True if at least one regression was detected
        [[nodiscard]] bool HasRegressions(const std::vector<BenchmarkComparison>& comparisons) const
        {
            return std::any_of(comparisons.begin(), comparisons.end(),
                               [](const BenchmarkComparison& c) { return !c.passed; });
        }

        // ====================================================================
        // Console
        // ====================================================================

        /// @brief Get a human-readable status string for console debugging
        [[nodiscard]] std::string Console_GetStatus() const
        {
            std::ostringstream oss;
            oss << "[BenchmarkFramework] initialized=" << (m_initialized ? "true" : "false")
                << ", scenarios=" << m_scenarios.size();
            return oss.str();
        }

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

        /// @brief Find a registered scenario by name
        [[nodiscard]] IBenchmarkScenario* FindScenario(std::string_view name) const
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

        /// @brief Get current timestamp as ISO-8601 string
        [[nodiscard]] static std::string GetTimestamp()
        {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&time));
            return std::string(buf);
        }

        /// @brief Escape a string for JSON output
        [[nodiscard]] static std::string EscapeJson(const std::string& str)
        {
            std::string result;
            result.reserve(str.size());
            for (char c : str)
            {
                switch (c)
                {
                case '"':
                    result += "\\\"";
                    break;
                case '\\':
                    result += "\\\\";
                    break;
                case '\n':
                    result += "\\n";
                    break;
                case '\t':
                    result += "\\t";
                    break;
                default:
                    result += c;
                    break;
                }
            }
            return result;
        }

        /// @brief Extract a JSON string value from a line like: "key": "value"
        [[nodiscard]] static std::string ExtractJsonString(const std::string& line)
        {
            // Find second quoted string
            size_t firstClose = line.find('"', line.find('"') + 1);
            size_t valueStart = line.find('"', firstClose + 1);
            if (valueStart == std::string::npos)
            {
                return {};
            }
            size_t valueEnd = line.find('"', valueStart + 1);
            if (valueEnd == std::string::npos)
            {
                return {};
            }
            return line.substr(valueStart + 1, valueEnd - valueStart - 1);
        }

        /// @brief Trim whitespace from both ends
        [[nodiscard]] static std::string Trim(const std::string& str)
        {
            size_t start = str.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
            {
                return {};
            }
            size_t end = str.find_last_not_of(" \t\r\n");
            return str.substr(start, end - start + 1);
        }

        /// @brief Write string content to a file
        static void WriteFile(std::string_view path, const std::string& content)
        {
            std::string pathStr(path);
            if (auto* file = std::fopen(pathStr.c_str(), "w"))
            {
                std::fwrite(content.data(), 1, content.size(), file);
                std::fclose(file);
            }
        }

        /// @brief Read entire file into a string
        [[nodiscard]] static std::string ReadFile(std::string_view path)
        {
            std::string pathStr(path);
            auto* file = std::fopen(pathStr.c_str(), "r");
            if (!file)
            {
                return {};
            }
            std::fseek(file, 0, SEEK_END);
            auto size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            std::string content(static_cast<size_t>(size), '\0');
            auto bytesRead = std::fread(content.data(), 1, static_cast<size_t>(size), file);
            std::fclose(file);
            if (bytesRead != static_cast<size_t>(size))
                content.resize(bytesRead);
            return content;
        }

        std::vector<std::unique_ptr<IBenchmarkScenario>> m_scenarios;
        bool m_initialized = false;
    };

} // namespace Spark
