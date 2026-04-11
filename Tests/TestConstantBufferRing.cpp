/**
 * @file TestConstantBufferRing.cpp
 * @brief Phase N — tests for the ConstantBufferRing orphan
 *
 * Exercises `Spark::Graphics::ConstantBufferRing` — a D3D11 ring
 * buffer allocator. The class is Windows-only because it wraps
 * `ID3D11Buffer` + `ID3D11DeviceContext` directly, so the class
 * body lives under `#ifdef SPARK_PLATFORM_WINDOWS` in the header.
 * This test file wraps its contents in the same guard so it compiles
 * to an empty translation unit on Linux / macOS — CI only picks
 * these tests up on the Windows jobs.
 *
 * The tests focus on the uninitialised-state contract and input
 * rejection (null device, zero capacity, allocate-without-begin,
 * etc.) because a full D3D11 device is not available in the test
 * framework.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "../SparkEngine/Source/Graphics/ConstantBufferRing.h"

using Spark::Graphics::CBAllocation;
using Spark::Graphics::ConstantBufferRing;

// =========================================================================
// Uninitialised state contract
// =========================================================================

TEST(ConstantBufferRing_DefaultIsUninitialised)
{
    ConstantBufferRing ring;
    EXPECT_FALSE(ring.IsInitialized());
    EXPECT_FALSE(ring.IsFrameActive());
    EXPECT_EQ(ring.GetMetrics().capacityBytes, 0u);
    EXPECT_EQ(ring.GetMetrics().usedThisFrame, 0u);
    EXPECT_EQ(ring.GetMetrics().allocationsThisFrame, 0u);
    EXPECT_EQ(ring.GetMetrics().peakUsageBytes, 0u);
    EXPECT_TRUE(ring.GetBuffer() == nullptr);
}

TEST(ConstantBufferRing_InitializeNullDeviceFails)
{
    ConstantBufferRing ring;
    EXPECT_FALSE(ring.Initialize(nullptr, 1024));
    EXPECT_FALSE(ring.IsInitialized());
}

TEST(ConstantBufferRing_ShutdownBeforeInitializeIsSafe)
{
    ConstantBufferRing ring;
    ring.Shutdown(); // no crash
    EXPECT_FALSE(ring.IsInitialized());
}

TEST(ConstantBufferRing_BeginFrameBeforeInitializeFails)
{
    ConstantBufferRing ring;
    // A null context would also be rejected, but the uninit check fires first.
    EXPECT_FALSE(ring.BeginFrame(nullptr));
}

TEST(ConstantBufferRing_EndFrameBeforeBeginIsSafe)
{
    ConstantBufferRing ring;
    ring.EndFrame(); // no crash
    EXPECT_FALSE(ring.IsFrameActive());
}

TEST(ConstantBufferRing_AllocateBeforeBeginFrameReturnsInvalid)
{
    ConstantBufferRing ring;
    CBAllocation alloc = ring.Allocate(256);
    EXPECT_FALSE(alloc.IsValid());
    EXPECT_TRUE(alloc.cpuAddress == nullptr);
}

TEST(ConstantBufferRing_AllocateZeroBytesReturnsInvalid)
{
    ConstantBufferRing ring;
    CBAllocation alloc = ring.Allocate(0);
    EXPECT_FALSE(alloc.IsValid());
}

// =========================================================================
// CBAllocation contract
// =========================================================================

TEST(ConstantBufferRing_DefaultCBAllocationIsInvalid)
{
    CBAllocation a;
    EXPECT_FALSE(a.IsValid());
    EXPECT_EQ(a.offset, 0u);
    EXPECT_EQ(a.size, 0u);
    EXPECT_TRUE(a.cpuAddress == nullptr);
}

TEST(ConstantBufferRing_Console_GetStatusUninitialised)
{
    ConstantBufferRing ring;
    std::wstring status = ring.Console_GetStatus();
    EXPECT_TRUE(status.find(L"Not initialized") != std::wstring::npos);
}

#endif // SPARK_PLATFORM_WINDOWS
