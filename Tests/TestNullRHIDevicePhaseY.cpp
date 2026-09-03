/**
 * @file TestNullRHIDevicePhaseY.cpp
 * @brief Phase Y Theme 3B tests for NullRHIDevice wire-up
 *
 * Theme 3B second phase. Validates that Phase Y's rewrite of
 * NullRHIDevice to return real stub resources, wire in
 * Spark::RHI::HandlePool for each resource type, and wire in
 * Spark::RHI::TransientBufferAllocator across BeginFrame / EndFrame
 * actually works end-to-end against the real class:
 *
 *   - CreateBuffer now returns a real NullBuffer (not nullptr) and
 *     the BufferPool reflects the new live count.
 *   - MapBuffer on a NullBuffer returns a writable CPU pointer.
 *   - CreateTexture / CreateShader / CreateSampler / CreatePipelineState
 *     populate their matching pools.
 *   - Initialize wires the TransientBufferAllocator — after
 *     NullRHIDevice::Initialize, GetTransientBuffers() reports real
 *     budgets and can accept allocations.
 *   - BeginFrame / EndFrame correctly map/unmap the transient buffers;
 *     allocations inside a frame return valid blocks; allocations
 *     outside a frame fail.
 *   - Shutdown clears all pools and tears down the transient allocator.
 *   - Reinitializing after Shutdown works.
 */

#include "TestFramework.h"
#include "Graphics/RHI/NullRHIDevice.h"

#include <cstdint>
#include <cstring>

namespace
{

    // Each test runs against a fresh local device so there is no
    // interference from the singleton instance or prior test state.
    struct PhaseYFixture
    {
        Spark::RHI::NullRHIDevice device;
        PhaseYFixture()
        {
            Spark::RHI::RHIDeviceDesc desc;
            device.Initialize(desc);
        }
        ~PhaseYFixture() { device.Shutdown(); }
    };

} // namespace

// ============================================================================
// Real stub resources (no more nullptr)
// ============================================================================

TEST(NullRHIDevicePhaseY_CreateBufferReturnsRealBuffer)
{
    PhaseYFixture fx;
    Spark::RHI::RHIBufferDesc desc;
    desc.size = 1024;
    desc.stride = 16;
    desc.usage = Spark::RHI::RHIBufferUsage::Vertex;
    desc.access = Spark::RHI::RHIBufferAccess::Dynamic;

    auto buf = fx.device.CreateBuffer(desc);
    EXPECT_TRUE(buf != nullptr);
    EXPECT_TRUE(buf->IsValid());
    EXPECT_EQ(buf->GetSize(), static_cast<uint64_t>(1024));
    EXPECT_EQ(buf->GetStride(), static_cast<uint32_t>(16));
}

TEST(NullRHIDevicePhaseY_MapBufferReturnsWritablePointer)
{
    PhaseYFixture fx;
    Spark::RHI::RHIBufferDesc desc;
    desc.size = 256;

    auto buf = fx.device.CreateBuffer(desc);
    void* ptr = fx.device.MapBuffer(buf.get());
    ASSERT_TRUE(ptr != nullptr);

    // Prove the pointer is writable by memset'ing the whole range.
    std::memset(ptr, 0xCD, 256);
    // Read back through the CPU-visible storage.
    const uint8_t* bytes = static_cast<const uint8_t*>(ptr);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>(0xCD));
    EXPECT_EQ(bytes[255], static_cast<uint8_t>(0xCD));

    fx.device.UnmapBuffer(buf.get());
}

TEST(NullRHIDevicePhaseY_CreateTextureReturnsRealTexture)
{
    PhaseYFixture fx;
    Spark::RHI::RHITextureDesc desc;
    desc.width = 64;
    desc.height = 32;
    desc.format = Spark::RHI::PixelFormat::R8G8B8A8_UNORM;

    auto tex = fx.device.CreateTexture(desc);
    EXPECT_TRUE(tex != nullptr);
    EXPECT_TRUE(tex->IsValid());
    EXPECT_EQ(tex->GetWidth(), static_cast<uint32_t>(64));
    EXPECT_EQ(tex->GetHeight(), static_cast<uint32_t>(32));
}

TEST(NullRHIDevicePhaseY_CreateShaderReturnsRealShader)
{
    PhaseYFixture fx;
    Spark::RHI::RHIShaderDesc desc;
    desc.stage = Spark::RHI::RHIShaderStage::Pixel;
    desc.entryPoint = "PS_Main";

    auto shader = fx.device.CreateShader(desc);
    EXPECT_TRUE(shader != nullptr);
    EXPECT_TRUE(shader->IsValid());
    EXPECT_EQ(static_cast<int>(shader->GetStage()), static_cast<int>(Spark::RHI::RHIShaderStage::Pixel));
    EXPECT_EQ(shader->GetEntryPoint(), std::string("PS_Main"));
}

TEST(NullRHIDevicePhaseY_CreateSamplerReturnsRealSampler)
{
    PhaseYFixture fx;
    Spark::RHI::RHISamplerDesc desc;
    auto sampler = fx.device.CreateSampler(desc);
    EXPECT_TRUE(sampler != nullptr);
    EXPECT_TRUE(sampler->IsValid());
}

TEST(NullRHIDevicePhaseY_CreatePipelineReturnsRealPipeline)
{
    PhaseYFixture fx;
    Spark::RHI::RHIPipelineStateDesc desc;
    auto pipeline = fx.device.CreatePipelineState(desc, nullptr, nullptr);
    EXPECT_TRUE(pipeline != nullptr);
    EXPECT_TRUE(pipeline->IsValid());
}

// ============================================================================
// HandlePool wire-up
// ============================================================================

TEST(NullRHIDevicePhaseY_BufferPoolTracksCreates)
{
    PhaseYFixture fx;
    // Initialize already created two buffers for the transient allocator.
    const uint32_t poolCountBefore = fx.device.GetBufferPool().Count();

    Spark::RHI::RHIBufferDesc desc;
    desc.size = 256;
    auto a = fx.device.CreateBuffer(desc);
    auto b = fx.device.CreateBuffer(desc);
    auto c = fx.device.CreateBuffer(desc);

    const uint32_t poolCountAfter = fx.device.GetBufferPool().Count();
    EXPECT_EQ(poolCountAfter - poolCountBefore, static_cast<uint32_t>(3));
}

TEST(NullRHIDevicePhaseY_TexturePoolTracksCreates)
{
    PhaseYFixture fx;
    const uint32_t before = fx.device.GetTexturePool().Count();

    Spark::RHI::RHITextureDesc desc;
    desc.width = 32;
    desc.height = 32;
    fx.device.CreateTexture(desc);
    fx.device.CreateTexture(desc);

    EXPECT_EQ(fx.device.GetTexturePool().Count() - before, static_cast<uint32_t>(2));
}

TEST(NullRHIDevicePhaseY_ShaderPoolTracksCreates)
{
    PhaseYFixture fx;
    const uint32_t before = fx.device.GetShaderPool().Count();

    Spark::RHI::RHIShaderDesc desc;
    fx.device.CreateShader(desc);
    EXPECT_EQ(fx.device.GetShaderPool().Count() - before, static_cast<uint32_t>(1));
}

TEST(NullRHIDevicePhaseY_SamplerPoolTracksCreates)
{
    PhaseYFixture fx;
    const uint32_t before = fx.device.GetSamplerPool().Count();

    Spark::RHI::RHISamplerDesc desc;
    fx.device.CreateSampler(desc);
    fx.device.CreateSampler(desc);
    fx.device.CreateSampler(desc);
    EXPECT_EQ(fx.device.GetSamplerPool().Count() - before, static_cast<uint32_t>(3));
}

TEST(NullRHIDevicePhaseY_PipelinePoolTracksCreates)
{
    PhaseYFixture fx;
    const uint32_t before = fx.device.GetPipelinePool().Count();

    Spark::RHI::RHIPipelineStateDesc desc;
    fx.device.CreatePipelineState(desc, nullptr, nullptr);
    EXPECT_EQ(fx.device.GetPipelinePool().Count() - before, static_cast<uint32_t>(1));
}

TEST(NullRHIDevicePhaseY_ShutdownClearsAllPools)
{
    Spark::RHI::NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc devDesc;
    device.Initialize(devDesc);

    Spark::RHI::RHIBufferDesc bufDesc;
    bufDesc.size = 128;
    Spark::RHI::RHITextureDesc texDesc;
    Spark::RHI::RHIShaderDesc shDesc;
    Spark::RHI::RHISamplerDesc samDesc;
    Spark::RHI::RHIPipelineStateDesc pipDesc;

    device.CreateBuffer(bufDesc);
    device.CreateTexture(texDesc);
    device.CreateShader(shDesc);
    device.CreateSampler(samDesc);
    device.CreatePipelineState(pipDesc, nullptr, nullptr);

    // At least one entry per pool before Shutdown.
    EXPECT_TRUE(device.GetBufferPool().Count() > 0);
    EXPECT_TRUE(device.GetTexturePool().Count() > 0);
    EXPECT_TRUE(device.GetShaderPool().Count() > 0);
    EXPECT_TRUE(device.GetSamplerPool().Count() > 0);
    EXPECT_TRUE(device.GetPipelinePool().Count() > 0);

    device.Shutdown();

    EXPECT_EQ(device.GetBufferPool().Count(), static_cast<uint32_t>(0));
    EXPECT_EQ(device.GetTexturePool().Count(), static_cast<uint32_t>(0));
    EXPECT_EQ(device.GetShaderPool().Count(), static_cast<uint32_t>(0));
    EXPECT_EQ(device.GetSamplerPool().Count(), static_cast<uint32_t>(0));
    EXPECT_EQ(device.GetPipelinePool().Count(), static_cast<uint32_t>(0));
}

// ============================================================================
// TransientBufferAllocator wire-up
// ============================================================================

TEST(NullRHIDevicePhaseY_TransientAllocatorInitializedOnDeviceInit)
{
    PhaseYFixture fx;
    const auto& tb = fx.device.GetTransientBuffers();
    // The allocator's budgets must match the device's constructor
    // defaults: 64 KB vertex / 32 KB index.
    EXPECT_EQ(tb.GetVertexBudget(), static_cast<uint32_t>(64 * 1024));
    EXPECT_EQ(tb.GetIndexBudget(), static_cast<uint32_t>(32 * 1024));
}

TEST(NullRHIDevicePhaseY_BeginFrameAllocatesTransientVertices)
{
    PhaseYFixture fx;
    fx.device.BeginFrame();

    auto& tb = fx.device.GetTransientBuffers();
    auto alloc = tb.AllocateVertices(100, 16); // 1600 bytes
    EXPECT_TRUE(alloc.valid);
    EXPECT_EQ(alloc.sizeBytes, static_cast<uint32_t>(1600));
    EXPECT_TRUE(alloc.data != nullptr);

    // Writable?
    std::memset(alloc.data, 0xAB, alloc.sizeBytes);

    fx.device.EndFrame();
}

TEST(NullRHIDevicePhaseY_BeginFrameAllocatesTransientIndices)
{
    PhaseYFixture fx;
    fx.device.BeginFrame();

    auto& tb = fx.device.GetTransientBuffers();
    auto alloc = tb.AllocateIndices(256); // 1024 bytes
    EXPECT_TRUE(alloc.valid);
    EXPECT_EQ(alloc.sizeBytes, static_cast<uint32_t>(1024));

    fx.device.EndFrame();
}

TEST(NullRHIDevicePhaseY_AllocationOutsideFrameFails)
{
    PhaseYFixture fx;
    // No BeginFrame — mapped pointer should be null.
    auto& tb = fx.device.GetTransientBuffers();
    auto alloc = tb.AllocateVertices(16, 32);
    EXPECT_FALSE(alloc.valid);
}

TEST(NullRHIDevicePhaseY_FrameResetsTransientOffsets)
{
    PhaseYFixture fx;
    auto& tb = fx.device.GetTransientBuffers();

    fx.device.BeginFrame();
    tb.AllocateVertices(10, 32);
    EXPECT_TRUE(tb.GetVertexBytesUsed() > 0);
    fx.device.EndFrame();

    fx.device.BeginFrame();
    EXPECT_EQ(tb.GetVertexBytesUsed(), static_cast<uint32_t>(0));
    fx.device.EndFrame();
}

TEST(NullRHIDevicePhaseY_OverBudgetTransientAllocFails)
{
    PhaseYFixture fx;
    fx.device.BeginFrame();

    auto& tb = fx.device.GetTransientBuffers();
    // 64 KB vertex budget; try to allocate 128 KB.
    auto alloc = tb.AllocateVertices(4096, 32); // 131072 bytes
    EXPECT_FALSE(alloc.valid);

    fx.device.EndFrame();
}

// ============================================================================
// Reinitialize lifecycle
// ============================================================================

TEST(NullRHIDevicePhaseY_ReinitializeAfterShutdown)
{
    Spark::RHI::NullRHIDevice device;
    Spark::RHI::RHIDeviceDesc devDesc;

    device.Initialize(devDesc);
    EXPECT_TRUE(device.IsInitialized());

    Spark::RHI::RHIBufferDesc bd;
    bd.size = 64;
    device.CreateBuffer(bd);
    EXPECT_TRUE(device.GetBufferPool().Count() > 0);

    device.Shutdown();
    EXPECT_FALSE(device.IsInitialized());
    EXPECT_EQ(device.GetBufferPool().Count(), static_cast<uint32_t>(0));

    device.Initialize(devDesc);
    EXPECT_TRUE(device.IsInitialized());
    // After reinit, transient allocator has been re-initialized too;
    // BeginFrame/EndFrame should still work.
    device.BeginFrame();
    auto alloc = device.GetTransientBuffers().AllocateVertices(10, 16);
    EXPECT_TRUE(alloc.valid);
    device.EndFrame();

    device.Shutdown();
}

// ============================================================================
// Immediate command list still available
// ============================================================================

TEST(NullRHIDevicePhaseY_ImmediateCommandListWorks)
{
    PhaseYFixture fx;
    Spark::RHI::IRHICommandList* cmd = fx.device.GetImmediateCommandList();
    ASSERT_TRUE(cmd != nullptr);

    cmd->Begin();
    cmd->Draw(3, 0);
    cmd->Draw(6, 0);
    cmd->End();
    // Cast back to NullCommandList to query draw count.
    auto* nullCmd = static_cast<Spark::RHI::NullCommandList*>(cmd);
    EXPECT_EQ(nullCmd->GetDrawCallCount(), static_cast<uint32_t>(2));
}

TEST(NullRHIDevicePhaseY_SwapChainAndDeferredListAreFunctional)
{
    PhaseYFixture fx;
    Spark::RHI::RHISwapChainDesc desc;
    desc.width = 640;
    desc.height = 360;
    desc.bufferCount = 3;
    auto swapChain = fx.device.CreateSwapChain(desc);
    EXPECT_TRUE(swapChain != nullptr);
    EXPECT_TRUE(swapChain->GetBackBuffer() != nullptr);
    EXPECT_TRUE(swapChain->GetBackBuffer()->GetDesc().usage & Spark::RHI::RHITextureUsage::RenderTarget);
    EXPECT_TRUE(swapChain->Present(false));
    EXPECT_EQ(swapChain->GetCurrentBufferIndex(), 1u);
    EXPECT_TRUE(swapChain->Resize(800, 600));
    EXPECT_EQ(swapChain->GetWidth(), 800u);
    EXPECT_EQ(swapChain->GetHeight(), 600u);
    EXPECT_FALSE(swapChain->Resize(0, 600));

    auto deferred = fx.device.CreateDeferredCommandList();
    EXPECT_TRUE(deferred != nullptr);
    deferred->Begin();
    deferred->Draw(3);
    deferred->Dispatch(1, 1, 1);
    deferred->End();
    auto* nullDeferred = static_cast<Spark::RHI::NullCommandList*>(deferred.get());
    EXPECT_EQ(nullDeferred->GetDispatchCount(), 1u);
    deferred->Reset();
    EXPECT_EQ(nullDeferred->GetDrawCallCount(), 0u);
    EXPECT_EQ(nullDeferred->GetDispatchCount(), 0u);
    deferred->Dispatch(1, 1, 1);
    EXPECT_EQ(nullDeferred->GetDispatchCount(), 1u);
    deferred->Begin();
    EXPECT_EQ(nullDeferred->GetDispatchCount(), 0u);
    deferred->End();
    fx.device.ExecuteCommandList(deferred.get());
    EXPECT_EQ(fx.device.GetNullStats().commandListsExecuted, 1u);
}

TEST(NullRHIDevicePhaseY_UpdateBufferWritesWithinBounds)
{
    PhaseYFixture fx;
    Spark::RHI::RHIBufferDesc desc;
    desc.size = 8;
    auto buffer = fx.device.CreateBuffer(desc);
    const uint8_t update[3] = {4, 5, 6};
    fx.device.UpdateBuffer(buffer.get(), update, sizeof(update), 2);
    const auto* bytes = static_cast<const uint8_t*>(fx.device.MapBuffer(buffer.get()));
    ASSERT_TRUE(bytes != nullptr);
    EXPECT_EQ(bytes[1], 0u);
    EXPECT_EQ(bytes[2], 4u);
    EXPECT_EQ(bytes[4], 6u);
    EXPECT_EQ(bytes[5], 0u);
    fx.device.UnmapBuffer(buffer.get());
}
