// TestTransientBufferAllocator.cpp - Tests for per-frame transient buffer allocation
// Validates bump allocation, alignment, budget enforcement, and frame reset.

#include "TestFramework.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace TestTransientAllocator
{

    // Standalone reproduction of the allocator logic for CI testing
    struct TransientAllocation
    {
        void* data = nullptr;
        uint32_t offsetBytes = 0;
        uint32_t sizeBytes = 0;
        bool valid = false;
    };

    class MockTransientAllocator
    {
      public:
        explicit MockTransientAllocator(uint32_t vertexBudget = 4096, uint32_t indexBudget = 2048)
            : m_vertexBudget(vertexBudget), m_indexBudget(indexBudget)
        {
            m_vertexData.resize(vertexBudget);
            m_indexData.resize(indexBudget);
        }

        void BeginFrame()
        {
            m_vertexOffset = 0;
            m_indexOffset = 0;
            m_mapped = true;
        }

        void EndFrame() { m_mapped = false; }

        TransientAllocation AllocateVertices(uint32_t vertexCount, uint32_t vertexStride)
        {
            uint32_t sizeBytes = vertexCount * vertexStride;
            uint32_t alignedOffset = (m_vertexOffset + 15) & ~15u;

            if (!m_mapped || alignedOffset + sizeBytes > m_vertexBudget)
                return {};

            TransientAllocation alloc;
            alloc.data = m_vertexData.data() + alignedOffset;
            alloc.offsetBytes = alignedOffset;
            alloc.sizeBytes = sizeBytes;
            alloc.valid = true;

            m_vertexOffset = alignedOffset + sizeBytes;
            return alloc;
        }

        TransientAllocation AllocateIndices(uint32_t indexCount, uint32_t indexStride = 4)
        {
            uint32_t sizeBytes = indexCount * indexStride;
            uint32_t alignedOffset = (m_indexOffset + 15) & ~15u;

            if (!m_mapped || alignedOffset + sizeBytes > m_indexBudget)
                return {};

            TransientAllocation alloc;
            alloc.data = m_indexData.data() + alignedOffset;
            alloc.offsetBytes = alignedOffset;
            alloc.sizeBytes = sizeBytes;
            alloc.valid = true;

            m_indexOffset = alignedOffset + sizeBytes;
            return alloc;
        }

        bool HasVertexSpace(uint32_t vertexCount, uint32_t vertexStride) const
        {
            uint32_t sizeBytes = vertexCount * vertexStride;
            uint32_t alignedOffset = (m_vertexOffset + 15) & ~15u;
            return m_mapped && (alignedOffset + sizeBytes <= m_vertexBudget);
        }

        uint32_t GetVertexBytesUsed() const { return m_vertexOffset; }
        uint32_t GetIndexBytesUsed() const { return m_indexOffset; }

      private:
        std::vector<uint8_t> m_vertexData;
        std::vector<uint8_t> m_indexData;
        uint32_t m_vertexBudget;
        uint32_t m_indexBudget;
        uint32_t m_vertexOffset = 0;
        uint32_t m_indexOffset = 0;
        bool m_mapped = false;
    };

} // namespace TestTransientAllocator

using namespace TestTransientAllocator;

TEST(TransientAllocator_BasicVertexAllocation)
{
    MockTransientAllocator alloc(4096, 2048);
    alloc.BeginFrame();

    auto result = alloc.AllocateVertices(100, 32); // 100 verts * 32 bytes = 3200 bytes
    EXPECT_TRUE(result.valid);
    EXPECT_NE(result.data, nullptr);
    EXPECT_EQ(result.sizeBytes, 3200u);
}

TEST(TransientAllocator_BasicIndexAllocation)
{
    MockTransientAllocator alloc(4096, 2048);
    alloc.BeginFrame();

    auto result = alloc.AllocateIndices(256, 4); // 256 indices * 4 bytes = 1024 bytes
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.sizeBytes, 1024u);
}

TEST(TransientAllocator_AllocationIsAligned)
{
    MockTransientAllocator alloc(4096, 2048);
    alloc.BeginFrame();

    // First allocation — offset should be 0 (already aligned)
    auto a1 = alloc.AllocateVertices(1, 5); // 5 bytes, odd size
    EXPECT_TRUE(a1.valid);
    EXPECT_EQ(a1.offsetBytes % 16, 0u);

    // Second allocation — should be aligned to 16 bytes
    auto a2 = alloc.AllocateVertices(1, 5);
    EXPECT_TRUE(a2.valid);
    EXPECT_EQ(a2.offsetBytes % 16, 0u);
    EXPECT_GE(a2.offsetBytes, 16u); // Should be at least 16 bytes past first
}

TEST(TransientAllocator_BudgetEnforced)
{
    MockTransientAllocator alloc(256, 128);
    alloc.BeginFrame();

    auto a1 = alloc.AllocateVertices(8, 32); // 256 bytes — fills budget exactly
    EXPECT_TRUE(a1.valid);

    // This should fail — no room left
    auto a2 = alloc.AllocateVertices(1, 32);
    EXPECT_FALSE(a2.valid);
}

TEST(TransientAllocator_FrameResetFreesSpace)
{
    MockTransientAllocator alloc(256, 128);
    alloc.BeginFrame();

    auto a1 = alloc.AllocateVertices(8, 32); // Fill budget
    EXPECT_TRUE(a1.valid);

    alloc.EndFrame();
    alloc.BeginFrame(); // New frame — reset

    // Should be able to allocate again
    auto a2 = alloc.AllocateVertices(8, 32);
    EXPECT_TRUE(a2.valid);
    EXPECT_EQ(a2.offsetBytes, 0u);
}

TEST(TransientAllocator_MultipleAllocationsTrackBytes)
{
    MockTransientAllocator alloc(4096, 2048);
    alloc.BeginFrame();

    alloc.AllocateVertices(10, 32); // 320 bytes
    EXPECT_EQ(alloc.GetVertexBytesUsed(), 320u);

    alloc.AllocateVertices(10, 32); // 320 more, but aligned
    // 320 aligned to 16 = 320, + 320 = 640
    EXPECT_EQ(alloc.GetVertexBytesUsed(), 640u);
}

TEST(TransientAllocator_CannotAllocateBeforeBeginFrame)
{
    MockTransientAllocator alloc(4096, 2048);
    // Don't call BeginFrame

    auto result = alloc.AllocateVertices(10, 32);
    EXPECT_FALSE(result.valid);
}

TEST(TransientAllocator_CannotAllocateAfterEndFrame)
{
    MockTransientAllocator alloc(4096, 2048);
    alloc.BeginFrame();
    alloc.EndFrame();

    auto result = alloc.AllocateVertices(10, 32);
    EXPECT_FALSE(result.valid);
}

TEST(TransientAllocator_HasSpaceCheck)
{
    MockTransientAllocator alloc(256, 128);
    alloc.BeginFrame();

    EXPECT_TRUE(alloc.HasVertexSpace(8, 32));  // 256 bytes — fits
    EXPECT_FALSE(alloc.HasVertexSpace(9, 32)); // 288 bytes — over budget

    alloc.AllocateVertices(4, 32);            // Use 128 bytes
    EXPECT_TRUE(alloc.HasVertexSpace(4, 32)); // 128 remaining — fits after alignment
}

TEST(TransientAllocator_WriteAndReadBack)
{
    MockTransientAllocator alloc(4096, 2048);
    alloc.BeginFrame();

    struct Vertex
    {
        float x, y, z;
    };

    auto result = alloc.AllocateVertices(3, sizeof(Vertex));
    EXPECT_TRUE(result.valid);

    auto* verts = static_cast<Vertex*>(result.data);
    verts[0] = {1.0f, 2.0f, 3.0f};
    verts[1] = {4.0f, 5.0f, 6.0f};
    verts[2] = {7.0f, 8.0f, 9.0f};

    EXPECT_NEAR(verts[0].x, 1.0f, 0.001f);
    EXPECT_NEAR(verts[2].z, 9.0f, 0.001f);
}
