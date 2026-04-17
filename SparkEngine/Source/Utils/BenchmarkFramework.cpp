#include "BenchmarkFramework.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <sstream>

namespace Spark
{

    BenchmarkFramework& BenchmarkFramework::GetInstance()
    {
        static BenchmarkFramework instance;
        return instance;
    }

    void BenchmarkFramework::Initialize()
    {
        m_scenarios.clear();
        m_initialized = true;
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

    void BenchmarkFramework::SaveBaseline(std::string_view path, const std::vector<BenchmarkResult>& results) const
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

    std::vector<BenchmarkBaseline> BenchmarkFramework::LoadBaseline(std::string_view path) const
    {
        std::vector<BenchmarkBaseline> baselines;
        std::string content = ReadFile(path);
        if (content.empty())
        {
            return baselines;
        }

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
                if (trimmed == "}," || trimmed == "}")
                {
                    inMetrics = false;
                }
            }
            else if (inMetrics)
            {
                auto colonPos = trimmed.find(':');
                if (colonPos != std::string::npos && trimmed.front() == '"')
                {
                    std::string metricName = trimmed.substr(1, trimmed.find('"', 1) - 1);
                    std::string valueStr = Trim(trimmed.substr(colonPos + 1));
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

    std::vector<BenchmarkComparison> BenchmarkFramework::CompareWithBaseline(
        const std::vector<BenchmarkResult>& results, const std::vector<BenchmarkBaseline>& baselines) const
    {
        std::vector<BenchmarkComparison> comparisons;

        for (const auto& result : results)
        {
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

    bool BenchmarkFramework::HasRegressions(const std::vector<BenchmarkComparison>& comparisons) const
    {
        return std::any_of(comparisons.begin(), comparisons.end(),
                           [](const BenchmarkComparison& c) { return !c.passed; });
    }

    std::string BenchmarkFramework::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "[BenchmarkFramework] initialized=" << (m_initialized ? "true" : "false")
            << ", scenarios=" << m_scenarios.size();
        return oss.str();
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

    std::string BenchmarkFramework::EscapeJson(const std::string& str)
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

    std::string BenchmarkFramework::ExtractJsonString(const std::string& line)
    {
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

    std::string BenchmarkFramework::Trim(const std::string& str)
    {
        size_t start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
        {
            return {};
        }
        size_t end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }

    void BenchmarkFramework::WriteFile(std::string_view path, const std::string& content)
    {
        std::string pathStr(path);
        if (auto* file = std::fopen(pathStr.c_str(), "w"))
        {
            std::fwrite(content.data(), 1, content.size(), file);
            std::fclose(file);
        }
    }

    std::string BenchmarkFramework::ReadFile(std::string_view path)
    {
        std::string pathStr(path);
        auto* file = std::fopen(pathStr.c_str(), "r");
        if (!file)
        {
            return {};
        }
        std::fseek(file, 0, SEEK_END);
        auto size = std::ftell(file);
        if (size < 0)
        {
            std::fclose(file);
            return {};
        }
        std::fseek(file, 0, SEEK_SET);
        std::string content(static_cast<size_t>(size), '\0');
        auto bytesRead = std::fread(content.data(), 1, static_cast<size_t>(size), file);
        std::fclose(file);
        if (bytesRead != static_cast<size_t>(size))
            content.resize(bytesRead);
        return content;
    }

} // namespace Spark
