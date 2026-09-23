/**
 * @file TestHEAD220NullRHILifetimeReal.cpp
 * @brief HEAD-220: NullRHIDevice live-resource accounting must follow resource lifetime.
 *
 * A packaged headless run relies on NullRHIDevice's per-type pools as its only
 * live-resource signal (leak and soak evidence). Before this fix every Create*
 * registered the new object in a fixed 256-slot pool and nothing ever released
 * the slot when the caller destroyed the object, so:
 *
 *   - the "live" count only ever rose, and a leak-free create/destroy loop was
 *     indistinguishable from a leak;
 *   - after 256 cumulative creations the pool saturated, later resources were
 *     silently untracked, and the count froze at 256 whether zero or a thousand
 *     objects were alive;
 *   - the pools kept dangling pointers to destroyed objects.
 *
 * These tests drive the real NullRHIDevice (no fakes) and pin the contract the
 * GPU backends already honour: destroying a resource returns its slot, the
 * count equals the number of live objects, and a resource that outlives the
 * device's Shutdown or the device itself is released safely.
 */

#include "TestFramework.h"

#include "Graphics/RHI/NullRHIDevice.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    using Spark::RHI::NullRHIDevice;

    Spark::RHI::RHIBufferDesc SmallBufferDesc()
    {
        Spark::RHI::RHIBufferDesc desc;
        desc.size = 64;
        desc.stride = 16;
        return desc;
    }

    Spark::RHI::RHITextureDesc SmallTextureDesc()
    {
        Spark::RHI::RHITextureDesc desc;
        desc.width = 4;
        desc.height = 4;
        desc.format = Spark::RHI::PixelFormat::R8G8B8A8_UNORM;
        return desc;
    }
} // namespace

TEST(NullRHI_Lifetime_DestroyReleasesEveryPoolSlot)
{
    NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc deviceDesc;
    ASSERT_TRUE(device.Initialize(deviceDesc));

    const uint32_t buffersBefore = device.GetBufferPool().Count();
    const uint32_t texturesBefore = device.GetTexturePool().Count();
    const uint32_t shadersBefore = device.GetShaderPool().Count();
    const uint32_t samplersBefore = device.GetSamplerPool().Count();
    const uint32_t pipelinesBefore = device.GetPipelinePool().Count();

    {
        auto buffer = device.CreateBuffer(SmallBufferDesc());
        auto texture = device.CreateTexture(SmallTextureDesc());
        auto wrapped = device.WrapNativeTexture(nullptr, SmallTextureDesc());
        auto shader = device.CreateShader(Spark::RHI::RHIShaderDesc{});
        auto sampler = device.CreateSampler(Spark::RHI::RHISamplerDesc{});
        auto pipeline = device.CreatePipelineState(Spark::RHI::RHIPipelineStateDesc{}, nullptr, nullptr);
        ASSERT_TRUE(buffer && texture && wrapped && shader && sampler && pipeline);

        EXPECT_EQ(device.GetBufferPool().Count(), buffersBefore + 1u);
        EXPECT_EQ(device.GetTexturePool().Count(), texturesBefore + 2u);
        EXPECT_EQ(device.GetShaderPool().Count(), shadersBefore + 1u);
        EXPECT_EQ(device.GetSamplerPool().Count(), samplersBefore + 1u);
        EXPECT_EQ(device.GetPipelinePool().Count(), pipelinesBefore + 1u);
    }

    // Every object above is destroyed; the live counts must return to baseline.
    EXPECT_EQ(device.GetBufferPool().Count(), buffersBefore);
    EXPECT_EQ(device.GetTexturePool().Count(), texturesBefore);
    EXPECT_EQ(device.GetShaderPool().Count(), shadersBefore);
    EXPECT_EQ(device.GetSamplerPool().Count(), samplersBefore);
    EXPECT_EQ(device.GetPipelinePool().Count(), pipelinesBefore);

    device.Shutdown();
}

TEST(NullRHI_Lifetime_SoakChurnKeepsLiveCountExact)
{
    // A headless soak streams resources in and out for hours. Cumulative
    // creations far beyond the pool capacity must never saturate tracking.
    NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc deviceDesc;
    ASSERT_TRUE(device.Initialize(deviceDesc));

    const uint32_t texturesBefore = device.GetTexturePool().Count();
    const uint32_t churn = NullRHIDevice::kPoolCapacity * 4u;
    for (uint32_t i = 0; i < churn; ++i)
    {
        auto texture = device.CreateTexture(SmallTextureDesc());
        ASSERT_TRUE(texture != nullptr);
    }
    EXPECT_EQ(device.GetTexturePool().Count(), texturesBefore);
    EXPECT_EQ(device.GetUntrackedResourceCount(), 0u);

    // Tracking still works after the churn: one live object is counted as one.
    auto survivor = device.CreateTexture(SmallTextureDesc());
    EXPECT_EQ(device.GetTexturePool().Count(), texturesBefore + 1u);
    survivor.reset();
    EXPECT_EQ(device.GetTexturePool().Count(), texturesBefore);

    device.Shutdown();
}

TEST(NullRHI_Lifetime_ManySimultaneousResourcesAreAllCounted)
{
    // A real FPS scene keeps more than 256 buffers alive at once. The live
    // count must equal the number of live objects, and anything the device
    // could not track must be reported rather than silently dropped.
    NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc deviceDesc;
    ASSERT_TRUE(device.Initialize(deviceDesc));

    const uint32_t buffersBefore = device.GetBufferPool().Count();
    const uint32_t live = 300;
    std::vector<std::unique_ptr<Spark::RHI::IRHIBuffer>> buffers;
    buffers.reserve(live);
    for (uint32_t i = 0; i < live; ++i)
        buffers.push_back(device.CreateBuffer(SmallBufferDesc()));

    EXPECT_EQ(device.GetBufferPool().Count(), buffersBefore + live);
    EXPECT_EQ(device.GetUntrackedResourceCount(), 0u);

    buffers.clear();
    EXPECT_EQ(device.GetBufferPool().Count(), buffersBefore);

    device.Shutdown();
}

TEST(NullRHI_Lifetime_PoolOverflowIsReportedNotHidden)
{
    // Past kPoolCapacity live objects the pool count becomes a floor. That must
    // be visible to a leak check, and must clear again once the extras die.
    NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc deviceDesc;
    ASSERT_TRUE(device.Initialize(deviceDesc));

    std::vector<std::unique_ptr<Spark::RHI::IRHITexture>> textures;
    textures.reserve(NullRHIDevice::kPoolCapacity + 2u);
    for (uint32_t i = 0; i < NullRHIDevice::kPoolCapacity + 2u; ++i)
        textures.push_back(device.CreateTexture(SmallTextureDesc()));

    EXPECT_EQ(device.GetTexturePool().Count(), NullRHIDevice::kPoolCapacity);
    EXPECT_EQ(device.GetUntrackedResourceCount(), 2u);

    textures.clear();
    EXPECT_EQ(device.GetTexturePool().Count(), 0u);
    EXPECT_EQ(device.GetUntrackedResourceCount(), 0u);

    device.Shutdown();
}

TEST(NullRHI_Lifetime_ResourceFromBeforeReinitializeDoesNotFreeNewSlot)
{
    // A resource created before Shutdown and destroyed after a re-Initialize
    // must not release the slot of a resource created after the re-Initialize.
    NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc deviceDesc;
    ASSERT_TRUE(device.Initialize(deviceDesc));

    auto stale = device.CreateTexture(SmallTextureDesc());
    ASSERT_TRUE(stale != nullptr);
    device.Shutdown();
    EXPECT_EQ(device.GetTexturePool().Count(), 0u);

    ASSERT_TRUE(device.Initialize(deviceDesc));
    auto fresh = device.CreateTexture(SmallTextureDesc());
    ASSERT_TRUE(fresh != nullptr);
    EXPECT_EQ(device.GetTexturePool().Count(), 1u);

    stale.reset();
    EXPECT_EQ(device.GetTexturePool().Count(), 1u);

    fresh.reset();
    EXPECT_EQ(device.GetTexturePool().Count(), 0u);
    device.Shutdown();
}

TEST(NullRHI_Lifetime_ResourceMayOutliveDevice)
{
    // Headless shutdown order is not guaranteed: a component can release its
    // buffer after the RHI device is gone. That must be a no-op, not a
    // use-after-free (ASAN-visible) or a crash.
    std::unique_ptr<Spark::RHI::IRHIBuffer> orphanBuffer;
    std::unique_ptr<Spark::RHI::IRHITexture> orphanTexture;
    {
        auto device = std::make_unique<NullRHIDevice>();
        Spark::RHI::RHIDeviceDesc deviceDesc;
        ASSERT_TRUE(device->Initialize(deviceDesc));
        orphanBuffer = device->CreateBuffer(SmallBufferDesc());
        orphanTexture = device->CreateTexture(SmallTextureDesc());
        device->Shutdown();
        // Destroy without Shutdown for the second resource path as well.
    }
    {
        auto device = std::make_unique<NullRHIDevice>();
        Spark::RHI::RHIDeviceDesc deviceDesc;
        ASSERT_TRUE(device->Initialize(deviceDesc));
        auto keep = device->CreateSampler(Spark::RHI::RHISamplerDesc{});
        device.reset(); // device destroyed while `keep` is alive, no Shutdown
        keep.reset();
    }
    orphanBuffer.reset();
    orphanTexture.reset();
    EXPECT_TRUE(orphanBuffer == nullptr);
    EXPECT_TRUE(orphanTexture == nullptr);
}
