/**
 * @file Test_graphics_rhi.cpp
 * @brief Hardening regression tests for the graphics-rhi lane.
 *
 * Covers two bugs fixed during the harden-fleet pass:
 *   1. TransientResourcePool::GarbageCollect() unsigned-underflow eviction when
 *      the frame index decreases (scene reset / replay seek).
 *   2. The row-pitch formula used by D3D11Device::UpdateTexture for
 *      block-compressed formats and non-zero mip levels (contract lock via the
 *      shared RHIFormatUtils helpers the fix relies on).
 */

#include "TestFramework.h"

#include "Graphics/RenderGraph/TransientResourcePool.h"
#include "Graphics/RHI/RHIFormatUtils.h"

#include <algorithm>

using Spark::Graphics::TransientResourceDesc;
using Spark::Graphics::TransientResourcePool;

// ============================================================================
// TransientResourcePool::GarbageCollect frame-index underflow guard
// ============================================================================

TEST(TransientPool_GarbageCollectFrameUnderflowDoesNotEvict)
{
    // Regression: before the guard, a decreased frame index made
    // (m_currentFrame - lastUsedFrame) underflow to a huge unsigned value,
    // so every idle resource was wrongly evicted on the next GC.
    auto& pool = TransientResourcePool::GetInstance();
    pool.Initialize(4);

    TransientResourceDesc desc;
    desc.width = 128;
    desc.height = 128;
    desc.format = 28; // R8G8B8A8_UNORM
    desc.flags = 0;

    pool.BeginFrame(100);
    uint64_t handle = pool.AcquireResource(desc);
    EXPECT_NE(handle, 0ull);
    pool.ReleaseResource(handle);

    // Frame index steps backward (e.g. new scene / replay seek).
    pool.BeginFrame(0);
    pool.GarbageCollect();

    // With the guard, the idle resource survives instead of being evicted by
    // an underflowed age. Before the fix this count was 0.
    EXPECT_EQ(pool.GetPooledResourceCount(), 1u);

    pool.Shutdown();
}

TEST(TransientPool_GarbageCollectEvictsGenuinelyIdle)
{
    // The guard must not break normal, forward-progressing eviction.
    auto& pool = TransientResourcePool::GetInstance();
    pool.Initialize(4);

    TransientResourceDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.format = 28;
    desc.flags = 0;

    pool.BeginFrame(1);
    uint64_t handle = pool.AcquireResource(desc);
    pool.ReleaseResource(handle);

    // Advance well past maxIdleFrames (4) so the resource is genuinely stale.
    pool.BeginFrame(10);
    pool.GarbageCollect();
    EXPECT_EQ(pool.GetPooledResourceCount(), 0u);

    pool.Shutdown();
}

TEST(TransientPool_GarbageCollectKeepsInUseResources)
{
    auto& pool = TransientResourcePool::GetInstance();
    pool.Initialize(2);

    TransientResourceDesc desc;
    desc.width = 32;
    desc.height = 32;
    desc.format = 28;
    desc.flags = 0;

    pool.BeginFrame(1);
    uint64_t handle = pool.AcquireResource(desc);
    EXPECT_NE(handle, 0ull);

    // Never released: still in use. Even far in the future it must not be GC'd.
    pool.BeginFrame(100);
    pool.GarbageCollect();
    EXPECT_EQ(pool.GetActiveResourceCount(), 1u);
    EXPECT_EQ(pool.GetPooledResourceCount(), 1u);

    pool.Shutdown();
}

// ============================================================================
// UpdateTexture row-pitch formula contract
// ============================================================================

namespace
{
    // Mirrors the exact pitch computation now used by D3D11Device::UpdateTexture.
    uint32_t ComputeRowPitch(Spark::RHI::PixelFormat fmt, uint32_t baseWidth, uint32_t mipLevel)
    {
        const uint32_t mipWidth = std::max(1u, baseWidth >> mipLevel);
        if (Spark::RHI::IsCompressedFormat(fmt))
        {
            return ((mipWidth + 3) / 4) * Spark::RHI::GetFormatSize(fmt);
        }
        return mipWidth * Spark::RHI::GetFormatSize(fmt);
    }
} // namespace

TEST(UpdateTexturePitch_HelperContract)
{
    using Spark::RHI::GetFormatSize;
    using Spark::RHI::IsCompressedFormat;
    using PF = Spark::RHI::PixelFormat;

    // The fix depends on these classifications and block sizes.
    EXPECT_TRUE(IsCompressedFormat(PF::BC1_UNORM));
    EXPECT_TRUE(IsCompressedFormat(PF::BC3_UNORM));
    EXPECT_FALSE(IsCompressedFormat(PF::R8G8B8A8_UNORM));

    EXPECT_EQ(GetFormatSize(PF::BC1_UNORM), 8u);  // 8 bytes / 4x4 block
    EXPECT_EQ(GetFormatSize(PF::BC3_UNORM), 16u); // 16 bytes / 4x4 block
    EXPECT_EQ(GetFormatSize(PF::R8G8B8A8_UNORM), 4u);
}

TEST(UpdateTexturePitch_UncompressedRespectsMipLevel)
{
    using PF = Spark::RHI::PixelFormat;

    // Base mip: 256 * 4 bytes.
    EXPECT_EQ(ComputeRowPitch(PF::R8G8B8A8_UNORM, 256, 0), 1024u);
    // Mip 2 (width 64): the pitch must shrink, not stay at the base width.
    EXPECT_EQ(ComputeRowPitch(PF::R8G8B8A8_UNORM, 256, 2), 256u);
}

TEST(UpdateTexturePitch_CompressedUsesBlockRows)
{
    using PF = Spark::RHI::PixelFormat;

    // BC3, 256 wide, base mip: (256/4) blocks * 16 = 64 * 16 = 1024.
    EXPECT_EQ(ComputeRowPitch(PF::BC3_UNORM, 256, 0), 1024u);
    // Mip 2 (width 64): (64/4) * 16 = 16 * 16 = 256.
    // The old (buggy) formula returned baseWidth * blockBytes = 256 * 16 = 4096.
    EXPECT_EQ(ComputeRowPitch(PF::BC3_UNORM, 256, 2), 256u);

    // Non-block-multiple width rounds up to whole blocks: ceil(65/4)=17 blocks.
    EXPECT_EQ(ComputeRowPitch(PF::BC1_UNORM, 65, 0), 17u * 8u);
}
