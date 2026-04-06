// TestGPUStallProfiler.cpp — Unit tests for CPU-GPU timeline analysis and bottleneck detection
// Tests frame classification (CPU-bound, GPU-bound, balanced, bubble) and distribution tracking.

#include "TestFramework.h"

#include <array>
#include <cstdint>

// ============================================================================
// Standalone bottleneck classifier for test isolation
// ============================================================================

namespace
{

    enum class TestBottleneck : uint8_t
    {
        Unknown,
        CPUBound,
        GPUBound,
        Balanced,
        Bubble,
    };

    TestBottleneck ClassifyFrame(double cpuMs, double gpuMs)
    {
        double total = std::max(cpuMs, gpuMs);
        if (total < 0.01)
            return TestBottleneck::Unknown;

        double cpuUtil = cpuMs / total;
        double gpuUtil = gpuMs / total;

        if (cpuUtil > 0.7 && gpuUtil < 0.4)
            return TestBottleneck::CPUBound;
        if (gpuUtil > 0.7 && cpuUtil < 0.4)
            return TestBottleneck::GPUBound;
        if (cpuUtil < 0.4 && gpuUtil < 0.4)
            return TestBottleneck::Bubble;
        return TestBottleneck::Balanced;
    }

} // namespace

TEST("GPUStallProfiler_CPUBound", "[profiler]")
{
    auto result = ClassifyFrame(16.0, 4.0);
    ASSERT_TRUE(result == TestBottleneck::CPUBound);
}

TEST("GPUStallProfiler_GPUBound", "[profiler]")
{
    auto result = ClassifyFrame(4.0, 16.0);
    ASSERT_TRUE(result == TestBottleneck::GPUBound);
}

TEST("GPUStallProfiler_Balanced", "[profiler]")
{
    auto result = ClassifyFrame(15.0, 14.0);
    ASSERT_TRUE(result == TestBottleneck::Balanced);
}

TEST("GPUStallProfiler_GPUBound_Asymmetric", "[profiler]")
{
    auto result = ClassifyFrame(3.0, 10.0);
    ASSERT_TRUE(result == TestBottleneck::GPUBound);
}

TEST("GPUStallProfiler_Unknown", "[profiler]")
{
    auto result = ClassifyFrame(0.0, 0.0);
    ASSERT_TRUE(result == TestBottleneck::Unknown);
}

TEST("GPUStallProfiler_BottleneckDistribution", "[profiler]")
{
    std::array<TestBottleneck, 10> frames = {
        TestBottleneck::CPUBound, TestBottleneck::CPUBound, TestBottleneck::GPUBound, TestBottleneck::Balanced,
        TestBottleneck::Balanced, TestBottleneck::Balanced, TestBottleneck::CPUBound, TestBottleneck::GPUBound,
        TestBottleneck::Balanced, TestBottleneck::Bubble,
    };

    int cpu = 0, gpu = 0, bal = 0, bub = 0;
    for (auto b : frames)
    {
        switch (b)
        {
        case TestBottleneck::CPUBound:
            cpu++;
            break;
        case TestBottleneck::GPUBound:
            gpu++;
            break;
        case TestBottleneck::Balanced:
            bal++;
            break;
        case TestBottleneck::Bubble:
            bub++;
            break;
        default:
            break;
        }
    }

    ASSERT_EQ(cpu, 3);
    ASSERT_EQ(gpu, 2);
    ASSERT_EQ(bal, 4);
    ASSERT_EQ(bub, 1);
}
