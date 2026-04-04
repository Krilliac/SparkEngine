// TestBenchmarkFramework.cpp - Tests for Spark::BenchmarkFramework
#include "TestFramework.h"
#include "Utils/BenchmarkFramework.h"

#include <cstdio>
#include <fstream>
#include <sstream>

// ============================================================================
// Stub scenario for testing
// ============================================================================

class StubBenchmarkScenario final : public Spark::IBenchmarkScenario
{
  public:
    explicit StubBenchmarkScenario(std::string name, double metricValue = 1.0)
        : m_name(std::move(name)), m_metricValue(metricValue)
    {
    }

    std::string_view GetName() const override { return m_name; }

    std::vector<Spark::BenchmarkMetric> Run() override { return {{"FrameTime", m_metricValue, "ms", true}}; }

    uint32_t GetIterationCount() const override { return 1; }

  private:
    std::string m_name;
    double m_metricValue;
};

// ============================================================================
// RegisterScenario
// ============================================================================

TEST(BenchmarkFramework_RegisterScenario)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    bench.RegisterScenario(std::make_unique<StubBenchmarkScenario>("TestScenario"));

    auto status = bench.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "scenarios=1");

    bench.Shutdown();
}

TEST(BenchmarkFramework_RegisterScenario_NullIgnored)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    bench.RegisterScenario(nullptr);

    auto status = bench.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "scenarios=0");

    bench.Shutdown();
}

TEST(BenchmarkFramework_RegisterScenario_Multiple)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    bench.RegisterScenario(std::make_unique<StubBenchmarkScenario>("Scenario_A"));
    bench.RegisterScenario(std::make_unique<StubBenchmarkScenario>("Scenario_B"));

    auto status = bench.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "scenarios=2");

    bench.Shutdown();
}

// ============================================================================
// RunAll returns results for registered scenarios
// ============================================================================

TEST(BenchmarkFramework_RunAll_ReturnsResults)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    bench.RegisterScenario(std::make_unique<StubBenchmarkScenario>("Physics_Test", 5.0));
    bench.RegisterScenario(std::make_unique<StubBenchmarkScenario>("Render_Test", 8.0));

    auto results = bench.RunAll();
    EXPECT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].scenarioName == "Physics_Test");
    EXPECT_TRUE(results[1].scenarioName == "Render_Test");

    // Each result should have metrics
    EXPECT_FALSE(results[0].metrics.empty());
    EXPECT_FALSE(results[1].metrics.empty());

    bench.Shutdown();
}

TEST(BenchmarkFramework_RunAll_EmptyWhenNoScenarios)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    auto results = bench.RunAll();
    EXPECT_TRUE(results.empty());

    bench.Shutdown();
}

TEST(BenchmarkFramework_RunAll_MetricsHaveValues)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    bench.RegisterScenario(std::make_unique<StubBenchmarkScenario>("ValueCheck", 3.14));

    auto results = bench.RunAll();
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].metrics.size(), 1u);
    EXPECT_NEAR(results[0].metrics[0].value, 3.14, 0.01);
    EXPECT_TRUE(results[0].metrics[0].name == "FrameTime");

    bench.Shutdown();
}

// ============================================================================
// CompareWithBaseline detects regressions
// ============================================================================

TEST(BenchmarkFramework_CompareWithBaseline_NoRegression)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    // Result matches baseline within tolerance
    Spark::BenchmarkResult result;
    result.scenarioName = "TestScene";
    result.metrics = {{"FrameTime", 10.0, "ms", true}};

    Spark::BenchmarkBaseline baseline;
    baseline.scenarioName = "TestScene";
    baseline.metrics = {{"FrameTime", 10.0}};
    baseline.tolerancePercent = 5.0f;

    auto comparisons = bench.CompareWithBaseline({result}, {baseline});
    EXPECT_EQ(comparisons.size(), 1u);
    EXPECT_TRUE(comparisons[0].passed);
    EXPECT_TRUE(comparisons[0].regressions.empty());

    bench.Shutdown();
}

TEST(BenchmarkFramework_CompareWithBaseline_DetectsRegression)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    // Result is 20% worse than baseline (lower is better, so higher = regression)
    Spark::BenchmarkResult result;
    result.scenarioName = "TestScene";
    result.metrics = {{"FrameTime", 12.0, "ms", true}};

    Spark::BenchmarkBaseline baseline;
    baseline.scenarioName = "TestScene";
    baseline.metrics = {{"FrameTime", 10.0}};
    baseline.tolerancePercent = 5.0f;

    auto comparisons = bench.CompareWithBaseline({result}, {baseline});
    EXPECT_EQ(comparisons.size(), 1u);
    EXPECT_FALSE(comparisons[0].passed);
    EXPECT_EQ(comparisons[0].regressions.size(), 1u);
    EXPECT_TRUE(comparisons[0].regressions[0].metricName == "FrameTime");
    EXPECT_GT(comparisons[0].regressions[0].percentChange, 5.0);

    bench.Shutdown();
}

TEST(BenchmarkFramework_CompareWithBaseline_NoMatchingBaseline)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    Spark::BenchmarkResult result;
    result.scenarioName = "UnknownScene";
    result.metrics = {{"FrameTime", 10.0, "ms", true}};

    Spark::BenchmarkBaseline baseline;
    baseline.scenarioName = "DifferentScene";
    baseline.metrics = {{"FrameTime", 10.0}};

    auto comparisons = bench.CompareWithBaseline({result}, {baseline});
    EXPECT_TRUE(comparisons.empty());

    bench.Shutdown();
}

// ============================================================================
// HasRegressions returns true when regression exists
// ============================================================================

TEST(BenchmarkFramework_HasRegressions_True)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    Spark::BenchmarkComparison failed;
    failed.scenarioName = "FailedScene";
    failed.passed = false;
    failed.regressions = {{"FrameTime", 10.0, 15.0, 50.0, 5.0}};

    EXPECT_TRUE(bench.HasRegressions({failed}));

    bench.Shutdown();
}

TEST(BenchmarkFramework_HasRegressions_False)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    Spark::BenchmarkComparison passed;
    passed.scenarioName = "PassedScene";
    passed.passed = true;

    EXPECT_FALSE(bench.HasRegressions({passed}));

    bench.Shutdown();
}

TEST(BenchmarkFramework_HasRegressions_EmptyList)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    EXPECT_FALSE(bench.HasRegressions({}));

    bench.Shutdown();
}

// ============================================================================
// SaveBaseline / LoadBaseline format
// ============================================================================

TEST(BenchmarkFramework_SaveLoad_Baseline_Roundtrip)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    // Create results to save
    Spark::BenchmarkResult result;
    result.scenarioName = "RoundtripTest";
    result.metrics = {{"FrameTime", 16.5, "ms", true}, {"Memory", 256.0, "MB", true}};

    std::string path = "/tmp/spark_bench_test_baseline.json";
    bench.SaveBaseline(path, {result});

    auto baselines = bench.LoadBaseline(path);
    EXPECT_EQ(baselines.size(), 1u);
    EXPECT_TRUE(baselines[0].scenarioName == "RoundtripTest");

    auto it = baselines[0].metrics.find("FrameTime");
    EXPECT_TRUE(it != baselines[0].metrics.end());
    EXPECT_NEAR(it->second, 16.5, 0.01);

    auto it2 = baselines[0].metrics.find("Memory");
    EXPECT_TRUE(it2 != baselines[0].metrics.end());
    EXPECT_NEAR(it2->second, 256.0, 0.01);

    // Cleanup
    std::remove(path.c_str());

    bench.Shutdown();
}

TEST(BenchmarkFramework_LoadBaseline_NonexistentFile)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    auto baselines = bench.LoadBaseline("/tmp/spark_bench_nonexistent_file.json");
    EXPECT_TRUE(baselines.empty());

    bench.Shutdown();
}

TEST(BenchmarkFramework_SaveBaseline_ContainsJSON)
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    Spark::BenchmarkResult result;
    result.scenarioName = "JsonCheck";
    result.metrics = {{"FPS", 60.0, "fps", false}};

    std::string path = "/tmp/spark_bench_json_check.json";
    bench.SaveBaseline(path, {result});

    // Read file and verify it contains expected JSON keys
    std::ifstream ifs(path);
    EXPECT_TRUE(ifs.is_open());
    if (ifs.is_open())
    {
        std::ostringstream oss;
        oss << ifs.rdbuf();
        std::string content = oss.str();

        EXPECT_STR_CONTAINS(content, "baselines");
        EXPECT_STR_CONTAINS(content, "JsonCheck");
        EXPECT_STR_CONTAINS(content, "FPS");
        EXPECT_STR_CONTAINS(content, "60");
    }

    std::remove(path.c_str());

    bench.Shutdown();
}
