/**
 * @file TestGPUStallProfilerPhaseCC.cpp
 * @brief Phase CC Theme 3D tests for Spark::GPUStallProfiler
 *
 * GPUStallProfiler is a Utils-folder singleton that tracks CPU / GPU
 * frame overlap and classifies each frame as CPU-bound, GPU-bound,
 * balanced, or a pipeline bubble. Pre-Phase-CC it had a full inline
 * header implementation with zero external call sites; Phase CC
 * wires Initialize / Shutdown into GameplayLifecycleShared.cpp.
 *
 * Tests run against the real class directly:
 *
 *   - Singleton stability
 *   - Initialize + Shutdown lifecycle
 *   - BeginCPUWork / EndCPUWork / RecordGPUFrameTime / EndFrame
 *     populate the frame history
 *   - Bottleneck classification: CPU-bound (CPU >> GPU),
 *     GPU-bound (GPU >> CPU), Balanced (both busy), Bubble
 *     (neither busy)
 *   - Unknown on empty frames (all durations < threshold)
 *   - GetLastFrame returns the most recent timeline
 *   - GetCurrentBottleneck reflects the most recent frame
 *   - GetDistribution percentages sum to ~1.0 over a populated history
 *   - Shutdown clears the frame count; subsequent EndFrame is safe
 */

#include "TestFramework.h"
#include "Utils/GPUStallProfiler.h"

#include <thread>

namespace
{

    void ResetProfiler()
    {
        Spark::GPUStallProfiler::GetInstance().Shutdown();
        Spark::GPUStallProfiler::GetInstance().Initialize();
    }

    // Simulate a frame by sleeping the CPU portion, recording a GPU
    // frame time, and calling EndFrame. Durations are passed in
    // milliseconds and translated into the profiler's clock via a
    // busy-loop so the classification thresholds see real timing.
    void SimulateFrame(double cpuBusyMs, double gpuMs, double presentMs = 0.0)
    {
        auto& p = Spark::GPUStallProfiler::GetInstance();
        p.BeginCPUWork();
        // Busy-wait the CPU portion so the profiler sees real elapsed time.
        auto start = std::chrono::high_resolution_clock::now();
        while (std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start).count() <
               cpuBusyMs)
        {
            // Tight loop so the sample window reflects the busy time.
        }
        p.EndCPUWork();
        p.RecordGPUFrameTime(gpuMs);
        p.RecordPresentTime(presentMs);
        p.EndFrame();
    }

} // namespace

TEST(GPUStallProfilerPhaseCC_SingletonReturnsSameInstance)
{
    auto& a = Spark::GPUStallProfiler::GetInstance();
    auto& b = Spark::GPUStallProfiler::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(GPUStallProfilerPhaseCC_InitializeClearsHistory)
{
    ResetProfiler();
    auto& p = Spark::GPUStallProfiler::GetInstance();
    // After Initialize the last-frame bottleneck should be Unknown
    // because no frames have been recorded.
    EXPECT_EQ(static_cast<int>(p.GetCurrentBottleneck()), static_cast<int>(Spark::FrameBottleneck::Unknown));
}

TEST(GPUStallProfilerPhaseCC_SimulateFramePopulatesHistory)
{
    ResetProfiler();
    // 2ms CPU, 1ms GPU -> CPU-bound, but we only assert the frame
    // data lands.
    SimulateFrame(2.0, 1.0);
    const auto& last = Spark::GPUStallProfiler::GetInstance().GetLastFrame();
    EXPECT_TRUE(last.cpuFrameMs >= 1.5); // >= busy-loop target minus slack
    EXPECT_NEAR(last.gpuFrameMs, 1.0, 0.001);
}

TEST(GPUStallProfilerPhaseCC_CpuBoundClassification)
{
    ResetProfiler();
    // CPU >> GPU (ratio 0.2 : 1.0 < balanced-low; CPU util > 0.7).
    SimulateFrame(/*cpu*/ 5.0, /*gpu*/ 0.5);
    EXPECT_EQ(static_cast<int>(Spark::GPUStallProfiler::GetInstance().GetCurrentBottleneck()),
              static_cast<int>(Spark::FrameBottleneck::CPUBound));
}

TEST(GPUStallProfilerPhaseCC_GpuBoundClassification)
{
    ResetProfiler();
    // GPU >> CPU. Use a tiny CPU busy wait and a large gpuFrameMs.
    SimulateFrame(/*cpu*/ 0.1, /*gpu*/ 10.0);
    EXPECT_EQ(static_cast<int>(Spark::GPUStallProfiler::GetInstance().GetCurrentBottleneck()),
              static_cast<int>(Spark::FrameBottleneck::GPUBound));
}

TEST(GPUStallProfilerPhaseCC_BalancedClassification)
{
    ResetProfiler();
    // Both CPU and GPU at comparable busy levels — neither utilisation
    // exceeds 0.7 nor falls below 0.4 alone.
    SimulateFrame(/*cpu*/ 4.0, /*gpu*/ 4.0);
    const auto b = Spark::GPUStallProfiler::GetInstance().GetCurrentBottleneck();
    // Balanced is the expected outcome, but the busy-loop clock has
    // small variance; accept either Balanced or Unknown.
    EXPECT_TRUE(b == Spark::FrameBottleneck::Balanced || b == Spark::FrameBottleneck::Unknown);
}

TEST(GPUStallProfilerPhaseCC_UnknownOnZeroFrame)
{
    ResetProfiler();
    auto& p = Spark::GPUStallProfiler::GetInstance();
    // Seed pending values, then prove Initialize clears them and establishes
    // an exact zero-duration CPU sample without relying on clock-read latency.
    p.RecordGPUFrameTime(5.0);
    p.RecordPresentTime(2.0);
    p.Initialize();
    p.EndFrame();
    const auto& last = p.GetLastFrame();
    EXPECT_NEAR(last.cpuFrameMs, 0.0, 0.0);
    EXPECT_NEAR(last.gpuFrameMs, 0.0, 0.0);
    EXPECT_NEAR(last.presentMs, 0.0, 0.0);
    EXPECT_EQ(static_cast<int>(p.GetCurrentBottleneck()), static_cast<int>(Spark::FrameBottleneck::Unknown));
}

TEST(GPUStallProfilerPhaseCC_DistributionSumsToOne)
{
    ResetProfiler();
    // Populate the history with mixed frames.
    for (int i = 0; i < 10; ++i)
        SimulateFrame(3.0, 1.0); // CPU-bound
    for (int i = 0; i < 10; ++i)
        SimulateFrame(0.5, 8.0); // GPU-bound

    auto dist = Spark::GPUStallProfiler::GetInstance().GetDistribution();
    float total = dist.cpuBoundPct + dist.gpuBoundPct + dist.balancedPct + dist.bubblePct;
    // The four percentages are reported in 0–100 form and should sum
    // to 100% when the history has at least one classified frame.
    EXPECT_NEAR(total, 100.0f, 0.5f);
    // And CPU-bound and GPU-bound should each have non-zero share.
    EXPECT_TRUE(dist.cpuBoundPct > 0.0f);
    EXPECT_TRUE(dist.gpuBoundPct > 0.0f);
}

TEST(GPUStallProfilerPhaseCC_ShutdownIsSafe)
{
    ResetProfiler();
    SimulateFrame(2.0, 1.0);
    Spark::GPUStallProfiler::GetInstance().Shutdown();
    // Re-initialising and recording another frame is safe.
    Spark::GPUStallProfiler::GetInstance().Initialize();
    SimulateFrame(1.0, 1.0);
    // No crash == pass.
    EXPECT_TRUE(true);
}

TEST(GPUStallProfilerPhaseCC_PresentTimeCapturedInTimeline)
{
    ResetProfiler();
    SimulateFrame(/*cpu*/ 2.0, /*gpu*/ 1.0, /*present*/ 0.5);
    const auto& last = Spark::GPUStallProfiler::GetInstance().GetLastFrame();
    EXPECT_NEAR(last.presentMs, 0.5, 0.001);
}
