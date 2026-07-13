# Benchmark Framework

The Benchmark Framework provides a structured way to define, run, and compare performance benchmarks against saved baselines. It detects regressions by comparing measured metrics against stored thresholds, making it suitable for CI integration and automated performance regression testing.

`BenchmarkFramework::Initialize()` is called during engine startup (in `GameplayLifecycleShared.cpp`) and starts **empty** by design — scenarios are registered by whoever wants to benchmark. Engine startup then calls `RegisterBuiltinScenarios()`, which adds two dependency-free, deterministic throughput canaries (`cpu.sort_1m`, `mem.churn_4k_x10k`) so `benchmark.run` has real regression signal out of the box; feature code registers domain-specific scenarios via `RegisterScenario()`. Runs are triggered on demand via the `benchmark.*` console commands (see [Console Commands](#console-commands)).

**Source:** `SparkEngine/Source/Utils/BenchmarkFramework.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `IBenchmarkScenario` | Abstract interface for defining a repeatable performance test scenario |
| `BenchmarkFramework` | Singleton that registers scenarios, runs them, saves/loads baselines, and detects regressions |
| `BenchmarkMetric` | A single named measurement from a benchmark run (value + unit + direction) |
| `BenchmarkResult` | Aggregated results from running one scenario across all iterations |
| `BenchmarkBaseline` | Stored reference values and tolerance for a scenario |
| `BenchmarkComparison` | Pass/fail result of comparing a scenario run against its baseline |
| `RegressionDetail` | Detailed information about a single metric that regressed |

## Key Types

### BenchmarkMetric

A single named metric captured during a benchmark iteration:

```cpp
struct BenchmarkMetric
{
    std::string name;          // Metric name (e.g. "FrameTime")
    double value = 0.0;        // Measured value
    std::string unit;          // Unit string (e.g. "ms", "MB", "count")
    bool lowerIsBetter = true; // Whether lower values indicate better performance
};
```

### BenchmarkResult

Results from running a complete benchmark scenario (averaged across iterations):

```cpp
struct BenchmarkResult
{
    std::string scenarioName;             // Name of the scenario that was run
    std::vector<BenchmarkMetric> metrics; // All collected metrics (averaged)
    uint32_t iterations = 0;             // Number of iterations executed
    std::string timestamp;               // ISO-8601 timestamp of the run
};
```

### BenchmarkBaseline

Stored reference values loaded from a baseline file:

```cpp
struct BenchmarkBaseline
{
    std::string scenarioName;              // Scenario this baseline belongs to
    std::map<std::string, double> metrics; // Metric name -> baseline value
    float tolerancePercent = 5.0f;         // Allowed deviation before flagging regression
};
```

### RegressionDetail and BenchmarkComparison

Comparison output with details on any regressions detected:

```cpp
struct RegressionDetail
{
    std::string metricName;     // Which metric regressed
    double baseline = 0.0;      // Expected baseline value
    double measured = 0.0;      // Actually measured value
    double percentChange = 0.0; // Percentage change from baseline
    double threshold = 0.0;     // Allowed threshold percentage
};

struct BenchmarkComparison
{
    std::string scenarioName;                  // Scenario that was compared
    bool passed = true;                        // True if no regressions detected
    std::vector<RegressionDetail> regressions; // Details of any regressions found
};
```

## Quick Start

### Writing a Benchmark Scenario

Implement `IBenchmarkScenario` to define a repeatable performance test. The framework calls `Setup()` once, then `Run()` for `GetIterationCount()` iterations, then `TearDown()` once. Metrics are averaged across all iterations.

```cpp
#include "Utils/BenchmarkFramework.h"

class PhysicsBenchmark : public Spark::IBenchmarkScenario
{
public:
    std::string_view GetName() const override { return "Physics_1000Bodies"; }

    void Setup() override
    {
        // Create 1000 physics bodies for the test
        m_world = CreatePhysicsWorld();
        for (int i = 0; i < 1000; ++i)
            m_world->AddBody(RandomBody());
    }

    std::vector<Spark::BenchmarkMetric> Run() override
    {
        auto start = std::chrono::high_resolution_clock::now();

        m_world->Step(1.0f / 60.0f);

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        return {
            {"StepTime", ms, "ms", true},             // lower is better
            {"ActiveBodies", 1000.0, "count", false}   // higher is better
        };
    }

    void TearDown() override
    {
        m_world.reset();
    }

    uint32_t GetIterationCount() const override { return 20; }

private:
    std::unique_ptr<PhysicsWorld> m_world;
};
```

### Registering and Running Benchmarks

```cpp
auto& bench = Spark::BenchmarkFramework::GetInstance();
bench.Initialize();

// Register one or more scenarios
bench.RegisterScenario(std::make_unique<PhysicsBenchmark>());
bench.RegisterScenario(std::make_unique<RenderBenchmark>());

// Run all registered scenarios
auto results = bench.RunAll();

// Or run a single scenario by name
auto singleResult = bench.RunScenario("Physics_1000Bodies");

for (const auto& metric : singleResult.metrics)
{
    std::print("  {}: {} {}\n", metric.name, metric.value, metric.unit);
}
```

### Saving and Loading Baselines

Baselines are stored as simple JSON files. Save after a known-good run, then compare future runs against the baseline.

```cpp
// Save baseline after a reference run
bench.SaveBaseline("baselines.json", results);

// Later: load baseline and compare
auto baselines = bench.LoadBaseline("baselines.json");
auto comparisons = bench.CompareWithBaseline(results, baselines);

if (bench.HasRegressions(comparisons))
{
    for (const auto& comp : comparisons)
    {
        for (const auto& reg : comp.regressions)
        {
            std::print("REGRESSION: {} in {} -- baseline: {:.2f}, measured: {:.2f} ({:+.1f}%)\n",
                       reg.metricName, comp.scenarioName,
                       reg.baseline, reg.measured, reg.percentChange);
        }
    }
}
```

## Configuration

### Tolerance Thresholds

Each `BenchmarkBaseline` has a `tolerancePercent` field (default **5.0%**) that controls how much deviation is allowed before a regression is flagged:

- For **lower-is-better** metrics (e.g., frame time): a positive percent change exceeding the tolerance triggers a regression.
- For **higher-is-better** metrics (e.g., throughput): a negative percent change exceeding the tolerance triggers a regression.

```cpp
// Custom tolerance for a specific baseline
BenchmarkBaseline baseline;
baseline.scenarioName = "CriticalPath";
baseline.metrics["FrameTime"] = 16.0;
baseline.tolerancePercent = 2.0f;  // Stricter: only 2% allowed
```

### Iteration Count

Override `GetIterationCount()` in your scenario to control averaging. More iterations reduce noise but increase benchmark duration:

```cpp
uint32_t GetIterationCount() const override { return 100; } // Default is 10
```

### Baseline JSON Format

Baselines are stored in a straightforward JSON structure:

```json
{
  "baselines": [
    {
      "scenario": "Physics_1000Bodies",
      "metrics": {
        "StepTime": 2.45,
        "ActiveBodies": 1000.0
      },
      "tolerance": 5.0
    },
    {
      "scenario": "Render_ShadowMaps",
      "metrics": {
        "FrameTime": 8.12,
        "DrawCalls": 150.0
      },
      "tolerance": 10.0
    }
  ]
}
```

## Console Commands

The framework exposes a console status method for runtime debugging:

```cpp
auto status = bench.Console_GetStatus();
// Output: "[BenchmarkFramework] initialized=true, scenarios=3"
```

## CI Integration

### Automated Regression Detection

Use the benchmark framework in CI pipelines to catch performance regressions before merge:

```cpp
int main()
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();

    bench.RegisterScenario(std::make_unique<PhysicsBenchmark>());
    bench.RegisterScenario(std::make_unique<RenderBenchmark>());
    bench.RegisterScenario(std::make_unique<ECSBenchmark>());

    auto results = bench.RunAll();
    auto baselines = bench.LoadBaseline("Tests/baselines.json");
    auto comparisons = bench.CompareWithBaseline(results, baselines);

    if (bench.HasRegressions(comparisons))
    {
        for (const auto& comp : comparisons)
        {
            for (const auto& reg : comp.regressions)
            {
                std::print(stderr, "FAIL: {} {} {:+.1f}% (limit {}%)\n",
                           comp.scenarioName, reg.metricName,
                           reg.percentChange, reg.threshold);
            }
        }
        return 1;  // Non-zero exit code fails CI
    }

    std::println("All benchmarks passed.");
    return 0;
}
```

### Updating Baselines

After intentional performance changes (e.g., adding a new rendering pass), update baselines:

```cpp
auto results = bench.RunAll();
bench.SaveBaseline("Tests/baselines.json", results);
```

## Integration

The Benchmark Framework is a standalone utility that does not depend on other engine systems at runtime. It integrates with:

- **CTest / Unit Tests** -- Benchmark scenarios can be wrapped in CTest cases for automated CI runs.
- **Engine Console** -- `Console_GetStatus()` reports framework state to `SimpleConsole`.
- **Golden Image Testing** -- Use alongside `GoldenImageTestRunner` for combined performance and visual regression testing.
- **Profiler** -- Benchmark scenarios can use `Spark::Profiler` internally to collect fine-grained timing data.

```cpp
// Example: wrapping a benchmark in a CTest test case
TEST_CASE("Performance regression check")
{
    auto& bench = Spark::BenchmarkFramework::GetInstance();
    bench.Initialize();
    bench.RegisterScenario(std::make_unique<PhysicsBenchmark>());

    auto results = bench.RunAll();
    auto baselines = bench.LoadBaseline("TestData/baselines.json");
    auto comparisons = bench.CompareWithBaseline(results, baselines);

    REQUIRE_FALSE(bench.HasRegressions(comparisons));
    bench.Shutdown();
}
```

## API Reference

### IBenchmarkScenario (Interface)

| Method | Description |
|--------|-------------|
| `GetName() -> string_view` | Unique name for this scenario |
| `Setup()` | One-time setup before iterations begin (default: no-op) |
| `Run() -> vector<BenchmarkMetric>` | Execute one iteration and return metrics |
| `TearDown()` | One-time teardown after all iterations (default: no-op) |
| `GetIterationCount() -> uint32_t` | Number of iterations to run (default: 10) |

### BenchmarkFramework (Singleton)

| Method | Description |
|--------|-------------|
| `GetInstance() -> BenchmarkFramework&` | Access the singleton |
| `Initialize()` | Clear scenarios and mark as initialized |
| `Shutdown()` | Release all scenarios and reset state |
| `RegisterScenario(unique_ptr<IBenchmarkScenario>)` | Register a benchmark scenario |
| `RunAll() -> vector<BenchmarkResult>` | Run all registered scenarios |
| `RunScenario(string_view) -> BenchmarkResult` | Run a single scenario by name |
| `SaveBaseline(string_view, vector<BenchmarkResult>)` | Save results as baseline JSON |
| `LoadBaseline(string_view) -> vector<BenchmarkBaseline>` | Load baselines from JSON |
| `CompareWithBaseline(results, baselines) -> vector<BenchmarkComparison>` | Compare results against baselines |
| `HasRegressions(vector<BenchmarkComparison>) -> bool` | Check if any comparisons failed |
| `Console_GetStatus() -> string` | Human-readable status string |

## Thread Safety

- `BenchmarkFramework` is **not thread-safe**. All calls to `RegisterScenario()`, `RunAll()`, `RunScenario()`, and baseline I/O must happen on the same thread.
- Individual `IBenchmarkScenario::Run()` implementations may use threads internally (e.g., multithreaded physics simulation), but the framework itself is single-threaded.
- `GetInstance()` uses a function-local static and is safe for concurrent first-access under C++11 magic-statics guarantees.

## Console Commands

Registered during engine startup (`BenchmarkFramework::RegisterConsoleCommands()`):

| Command | Description |
|---------|-------------|
| `benchmark.status` | Show initialization state and registered scenario count |
| `benchmark.run` | Run all registered scenarios and print averaged metrics per scenario |

The built-in canaries (`cpu.sort_1m`, `mem.churn_4k_x10k`) are covered by `Tests/TestBenchmarkFramework.cpp` (`BenchmarkFramework_BuiltinScenariosRunAndProduceMetrics`).

## See Also

- [[Golden-Image-Testing]] -- Visual regression testing framework
- [[Profiler]] -- Fine-grained performance profiling
- [[Console-System]] -- Engine console integration
- [[Testing]] -- Unit test infrastructure and CTest setup
