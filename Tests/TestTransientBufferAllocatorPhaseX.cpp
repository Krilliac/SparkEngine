/**
 * @file TestTransientBufferAllocatorPhaseX.cpp
 * @brief Phase X real-class tests for Spark::RHI::TransientBufferAllocator
 *
 * Theme 3B phase 1 (second orphan). The existing
 * Tests/TestTransientBufferAllocator.cpp exercises a local
 * `MockTransientAllocator` in an anonymous namespace and never touches
 * the real class at `Graphics/RHI/TransientBufferAllocator.h`. Per the
 * @note on that header the allocator is a per-system utility owned by
 * render systems that need cheap transient vertex/index memory — no
 * engine-lifecycle singleton, so the Theme 3B activation is to lock
 * down the real class's contract against a fake RHI device.
 *
 * Coverage (against the real Spark::RHI::TransientBufferAllocator):
 *
 *   - Initialize(nullptr) returns false (early-out guard).
 *   - Initialize with a fake device creates vertex + index buffers
 *     matching the budget.
 *   - BeginFrame maps the buffers, AllocateVertices returns a valid
 *     allocation with the right offset + size.
 *   - AllocateIndices bumps the index offset.
 *   - 16-byte alignment is enforced (offsets are multiples of 16).
 *   - HasVertexSpace / HasIndexSpace report availability correctly.
 *   - Allocations past the budget return `valid=false` and do not
 *     advance the offset.
 *   - EndFrame unmaps buffers; allocations after EndFrame are invalid.
 *   - BeginFrame of a second frame resets the offsets to zero.
 *   - GetVertexBudget / GetIndexBudget report the constructor values.
 *   - Shutdown releases the underlying buffer objects.
 */

#include "TestFramework.h"
#include "Graphics/RHI/TransientBufferAllocator.h"
#include "Graphics/RHI/RHIResources.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

    // A minimal fake IRHIBuffer that owns a chunk of CPU memory so
    // MapBuffer can return a writable pointer. Implements the pure
    // virtuals from IRHIResource + IRHIBuffer with no-op / trivial
    // returns.
    class FakeBuffer : public Spark::RHI::IRHIBuffer
    {
      public:
        explicit FakeBuffer(const Spark::RHI::RHIBufferDesc& desc) : m_desc(desc), m_storage(desc.size, 0) {}

        // IRHIResource
        const std::string& GetDebugName() const override { return m_name; }
        void SetDebugName(const std::string& name) override { m_name = name; }
        bool IsValid() const override { return true; }

        // IRHIBuffer
        const Spark::RHI::RHIBufferDesc& GetDesc() const override { return m_desc; }
        uint64_t GetSize() const override { return m_desc.size; }
        uint32_t GetStride() const override { return m_desc.stride; }
        void* GetNativeHandle() const override { return nullptr; }

        void* GetCpuPointer() { return m_storage.data(); }

      private:
        Spark::RHI::RHIBufferDesc m_desc;
        std::vector<uint8_t> m_storage;
        std::string m_name;
    };

    // A minimal fake IRHIDevice that only implements the subset of
    // methods used by TransientBufferAllocator (CreateBuffer,
    // MapBuffer, UnmapBuffer) and leaves the rest as no-op stubs.
    class FakeDevice : public Spark::RHI::IRHIDevice
    {
      public:
        bool Initialize(const Spark::RHI::RHIDeviceDesc&) override { return true; }
        void Shutdown() override {}

        std::unique_ptr<Spark::RHI::IRHISwapChain> CreateSwapChain(const Spark::RHI::RHISwapChainDesc&) override
        {
            return nullptr;
        }

        std::unique_ptr<Spark::RHI::IRHIBuffer> CreateBuffer(const Spark::RHI::RHIBufferDesc& desc) override
        {
            ++m_buffersCreated;
            return std::make_unique<FakeBuffer>(desc);
        }

        std::unique_ptr<Spark::RHI::IRHITexture> CreateTexture(const Spark::RHI::RHITextureDesc&) override
        {
            return nullptr;
        }

        std::unique_ptr<Spark::RHI::IRHIShader> CreateShader(const Spark::RHI::RHIShaderDesc&) override
        {
            return nullptr;
        }

        std::unique_ptr<Spark::RHI::IRHISampler> CreateSampler(const Spark::RHI::RHISamplerDesc&) override
        {
            return nullptr;
        }

        std::unique_ptr<Spark::RHI::IRHIPipelineState> CreatePipelineState(const Spark::RHI::RHIPipelineStateDesc&,
                                                                           Spark::RHI::IRHIShader*,
                                                                           Spark::RHI::IRHIShader*) override
        {
            return nullptr;
        }

        std::unique_ptr<Spark::RHI::IRHITexture> WrapNativeTexture(void*, const Spark::RHI::RHITextureDesc&) override
        {
            return nullptr;
        }

        void* MapBuffer(Spark::RHI::IRHIBuffer* buffer) override
        {
            if (!buffer)
                return nullptr;
            ++m_mapCalls;
            return static_cast<FakeBuffer*>(buffer)->GetCpuPointer();
        }

        void UnmapBuffer(Spark::RHI::IRHIBuffer*) override { ++m_unmapCalls; }
        void UpdateBuffer(Spark::RHI::IRHIBuffer*, const void*, size_t, size_t) override {}
        void UpdateTexture(Spark::RHI::IRHITexture*, const void*, uint32_t, uint32_t) override {}

        Spark::RHI::IRHICommandList* GetImmediateCommandList() override { return nullptr; }
        std::unique_ptr<Spark::RHI::IRHICommandList> CreateDeferredCommandList() override { return nullptr; }
        void ExecuteCommandList(Spark::RHI::IRHICommandList*) override {}

        void BeginFrame() override {}
        void EndFrame() override {}
        void WaitForIdle() override {}

        Spark::RHI::GraphicsBackend GetBackendType() const override { return Spark::RHI::GraphicsBackend::None; }
        const Spark::RHI::RHIDeviceCapabilities& GetCapabilities() const override { return m_caps; }
        const Spark::RHI::RHIStatistics& GetStatistics() const override { return m_stats; }
        void ResetStatistics() override {}
        std::string GetDeviceInfo() const override { return "FakePhaseXDevice"; }

        uint32_t GetBuffersCreated() const { return m_buffersCreated; }
        uint32_t GetMapCalls() const { return m_mapCalls; }
        uint32_t GetUnmapCalls() const { return m_unmapCalls; }

      private:
        Spark::RHI::RHIDeviceCapabilities m_caps;
        Spark::RHI::RHIStatistics m_stats;
        uint32_t m_buffersCreated = 0;
        uint32_t m_mapCalls = 0;
        uint32_t m_unmapCalls = 0;
    };

} // namespace

// ============================================================================
// Initialize guards
// ============================================================================

TEST(TransientBufferAllocatorPhaseX_InitializeNullptrFails)
{
    Spark::RHI::TransientBufferAllocator alloc(1024, 512);
    EXPECT_FALSE(alloc.Initialize(nullptr));
}

TEST(TransientBufferAllocatorPhaseX_InitializeSucceedsWithFakeDevice)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(1024, 512);
    EXPECT_TRUE(alloc.Initialize(&device));
    // Two buffers created: vertex + index.
    EXPECT_EQ(device.GetBuffersCreated(), static_cast<uint32_t>(2));
    EXPECT_EQ(alloc.GetVertexBudget(), static_cast<uint32_t>(1024));
    EXPECT_EQ(alloc.GetIndexBudget(), static_cast<uint32_t>(512));
    alloc.Shutdown(&device);
}

// ============================================================================
// Begin / Allocate / End cycle
// ============================================================================

TEST(TransientBufferAllocatorPhaseX_AllocateVerticesReturnsValidBlock)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(4096, 2048);
    alloc.Initialize(&device);
    alloc.BeginFrame(&device);

    auto a = alloc.AllocateVertices(10, 32); // 320 bytes
    EXPECT_TRUE(a.valid);
    EXPECT_TRUE(a.buffer != nullptr);
    EXPECT_TRUE(a.data != nullptr);
    EXPECT_EQ(a.sizeBytes, static_cast<uint32_t>(320));
    EXPECT_EQ(a.offsetBytes, static_cast<uint32_t>(0));

    // The allocation window advances.
    EXPECT_EQ(alloc.GetVertexBytesUsed(), static_cast<uint32_t>(320));

    // The returned data pointer is writable (this exercises the
    // CPU-visible FakeBuffer backing).
    std::memset(a.data, 0xAB, a.sizeBytes);

    alloc.EndFrame(&device);
    alloc.Shutdown(&device);
}

TEST(TransientBufferAllocatorPhaseX_AllocateIndicesAdvancesIndexOffset)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(4096, 2048);
    alloc.Initialize(&device);
    alloc.BeginFrame(&device);

    auto a = alloc.AllocateIndices(64); // 64 * 4 = 256 bytes
    EXPECT_TRUE(a.valid);
    EXPECT_EQ(a.sizeBytes, static_cast<uint32_t>(256));
    EXPECT_EQ(alloc.GetIndexBytesUsed(), static_cast<uint32_t>(256));

    alloc.EndFrame(&device);
    alloc.Shutdown(&device);
}

TEST(TransientBufferAllocatorPhaseX_SixteenByteAlignment)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(4096, 2048);
    alloc.Initialize(&device);
    alloc.BeginFrame(&device);

    auto a = alloc.AllocateVertices(1, 1); // 1 byte, unaligned size
    auto b = alloc.AllocateVertices(1, 1);
    EXPECT_TRUE(a.valid);
    EXPECT_TRUE(b.valid);
    EXPECT_EQ(a.offsetBytes % 16u, static_cast<uint32_t>(0));
    EXPECT_EQ(b.offsetBytes % 16u, static_cast<uint32_t>(0));
    // Second allocation must start at a 16-byte boundary past the
    // first, not packed adjacent.
    EXPECT_TRUE(b.offsetBytes >= 16);

    alloc.EndFrame(&device);
    alloc.Shutdown(&device);
}

// ============================================================================
// Budget enforcement
// ============================================================================

TEST(TransientBufferAllocatorPhaseX_OverBudgetAllocationFails)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(256, 128);
    alloc.Initialize(&device);
    alloc.BeginFrame(&device);

    // 300 bytes > 256 budget
    auto a = alloc.AllocateVertices(30, 10);
    EXPECT_FALSE(a.valid);
    EXPECT_EQ(a.sizeBytes, static_cast<uint32_t>(0));
    // Over-budget must not advance the offset.
    EXPECT_EQ(alloc.GetVertexBytesUsed(), static_cast<uint32_t>(0));

    alloc.EndFrame(&device);
    alloc.Shutdown(&device);
}

TEST(TransientBufferAllocatorPhaseX_HasVertexSpaceReportsAvailability)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(256, 128);
    alloc.Initialize(&device);
    alloc.BeginFrame(&device);

    EXPECT_TRUE(alloc.HasVertexSpace(1, 100));
    EXPECT_FALSE(alloc.HasVertexSpace(1, 512)); // over budget

    alloc.EndFrame(&device);
    alloc.Shutdown(&device);
}

TEST(TransientBufferAllocatorPhaseX_HasIndexSpaceReportsAvailability)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(256, 128);
    alloc.Initialize(&device);
    alloc.BeginFrame(&device);

    EXPECT_TRUE(alloc.HasIndexSpace(10));   // 40 bytes <= 128
    EXPECT_FALSE(alloc.HasIndexSpace(100)); // 400 bytes > 128

    alloc.EndFrame(&device);
    alloc.Shutdown(&device);
}

// ============================================================================
// Map / Unmap bookkeeping
// ============================================================================

TEST(TransientBufferAllocatorPhaseX_BeginFrameMapsEndFrameUnmaps)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(1024, 512);
    alloc.Initialize(&device);

    EXPECT_EQ(device.GetMapCalls(), static_cast<uint32_t>(0));
    alloc.BeginFrame(&device);
    EXPECT_EQ(device.GetMapCalls(), static_cast<uint32_t>(2)); // vertex + index

    alloc.EndFrame(&device);
    EXPECT_EQ(device.GetUnmapCalls(), static_cast<uint32_t>(2));

    alloc.Shutdown(&device);
}

TEST(TransientBufferAllocatorPhaseX_AllocationAfterEndFrameInvalid)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(1024, 512);
    alloc.Initialize(&device);
    alloc.BeginFrame(&device);
    alloc.EndFrame(&device);

    // After EndFrame the mapped pointers are null — allocations must
    // be rejected.
    auto a = alloc.AllocateVertices(1, 16);
    EXPECT_FALSE(a.valid);
    auto b = alloc.AllocateIndices(4);
    EXPECT_FALSE(b.valid);

    alloc.Shutdown(&device);
}

// ============================================================================
// Frame reset
// ============================================================================

TEST(TransientBufferAllocatorPhaseX_BeginFrameResetsOffsets)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(4096, 2048);
    alloc.Initialize(&device);

    alloc.BeginFrame(&device);
    alloc.AllocateVertices(10, 32); // advances vertex offset
    alloc.AllocateIndices(20);      // advances index offset
    EXPECT_TRUE(alloc.GetVertexBytesUsed() > 0);
    EXPECT_TRUE(alloc.GetIndexBytesUsed() > 0);
    alloc.EndFrame(&device);

    alloc.BeginFrame(&device);
    EXPECT_EQ(alloc.GetVertexBytesUsed(), static_cast<uint32_t>(0));
    EXPECT_EQ(alloc.GetIndexBytesUsed(), static_cast<uint32_t>(0));
    alloc.EndFrame(&device);

    alloc.Shutdown(&device);
}

// ============================================================================
// Budget getters
// ============================================================================

TEST(TransientBufferAllocatorPhaseX_BudgetGettersMatchConstructor)
{
    Spark::RHI::TransientBufferAllocator alloc(8192, 4096);
    EXPECT_EQ(alloc.GetVertexBudget(), static_cast<uint32_t>(8192));
    EXPECT_EQ(alloc.GetIndexBudget(), static_cast<uint32_t>(4096));
}

TEST(TransientBufferAllocatorPhaseX_DefaultBudgetsArePositive)
{
    Spark::RHI::TransientBufferAllocator alloc;
    // Defaults: 4 MB vertex, 2 MB index.
    EXPECT_EQ(alloc.GetVertexBudget(), static_cast<uint32_t>(4 * 1024 * 1024));
    EXPECT_EQ(alloc.GetIndexBudget(), static_cast<uint32_t>(2 * 1024 * 1024));
}

// ============================================================================
// Shutdown lifecycle
// ============================================================================

TEST(TransientBufferAllocatorPhaseX_ShutdownReleasesBuffers)
{
    FakeDevice device;
    Spark::RHI::TransientBufferAllocator alloc(1024, 512);
    alloc.Initialize(&device);
    EXPECT_EQ(device.GetBuffersCreated(), static_cast<uint32_t>(2));

    alloc.Shutdown(&device);
    // After Shutdown, a fresh Initialize should create another pair
    // of buffers (total cumulative: 4).
    alloc.Initialize(&device);
    EXPECT_EQ(device.GetBuffersCreated(), static_cast<uint32_t>(4));
    alloc.Shutdown(&device);
}
