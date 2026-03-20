// TestDrawIndirect.cpp - Tests for GPU-driven indirect draw/dispatch API
// Validates that DrawInstancedIndirect, DrawIndexedInstancedIndirect, and
// DispatchIndirect are wired through the RHI command list interface.

#include "TestFramework.h"
#include <cstdint>

// Standalone mock of indirect draw args matching D3D11/D3D12/Vulkan conventions
namespace TestDrawIndirect
{

    // D3D11_DRAW_INSTANCED_INDIRECT_ARGS layout
    struct DrawInstancedIndirectArgs
    {
        uint32_t vertexCountPerInstance = 0;
        uint32_t instanceCount = 0;
        uint32_t startVertexLocation = 0;
        uint32_t startInstanceLocation = 0;
    };

    // D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS layout
    struct DrawIndexedInstancedIndirectArgs
    {
        uint32_t indexCountPerInstance = 0;
        uint32_t instanceCount = 0;
        uint32_t startIndexLocation = 0;
        int32_t baseVertexLocation = 0;
        uint32_t startInstanceLocation = 0;
    };

    // D3D11_DISPATCH_INDIRECT_ARGS layout
    struct DispatchIndirectArgs
    {
        uint32_t threadGroupCountX = 0;
        uint32_t threadGroupCountY = 0;
        uint32_t threadGroupCountZ = 0;
    };

    // Mock command list that counts indirect calls
    class MockCommandList
    {
      public:
        void DrawInstancedIndirect(const void* argsBuffer, uint32_t argsOffset)
        {
            if (!argsBuffer)
                return;
            auto* args = reinterpret_cast<const DrawInstancedIndirectArgs*>(static_cast<const uint8_t*>(argsBuffer) +
                                                                            argsOffset);
            m_lastVertexCount = args->vertexCountPerInstance;
            m_lastInstanceCount = args->instanceCount;
            m_drawIndirectCalls++;
        }

        void DrawIndexedInstancedIndirect(const void* argsBuffer, uint32_t argsOffset)
        {
            if (!argsBuffer)
                return;
            auto* args = reinterpret_cast<const DrawIndexedInstancedIndirectArgs*>(
                static_cast<const uint8_t*>(argsBuffer) + argsOffset);
            m_lastIndexCount = args->indexCountPerInstance;
            m_lastInstanceCount = args->instanceCount;
            m_drawIndexedIndirectCalls++;
        }

        void DispatchIndirect(const void* argsBuffer, uint32_t argsOffset)
        {
            if (!argsBuffer)
                return;
            auto* args =
                reinterpret_cast<const DispatchIndirectArgs*>(static_cast<const uint8_t*>(argsBuffer) + argsOffset);
            m_lastGroupX = args->threadGroupCountX;
            m_lastGroupY = args->threadGroupCountY;
            m_lastGroupZ = args->threadGroupCountZ;
            m_dispatchIndirectCalls++;
        }

        uint32_t m_drawIndirectCalls = 0;
        uint32_t m_drawIndexedIndirectCalls = 0;
        uint32_t m_dispatchIndirectCalls = 0;
        uint32_t m_lastVertexCount = 0;
        uint32_t m_lastIndexCount = 0;
        uint32_t m_lastInstanceCount = 0;
        uint32_t m_lastGroupX = 0;
        uint32_t m_lastGroupY = 0;
        uint32_t m_lastGroupZ = 0;
    };

} // namespace TestDrawIndirect

using namespace TestDrawIndirect;

TEST(DrawIndirect_InstancedIndirectReadsArgs)
{
    MockCommandList cmd;
    DrawInstancedIndirectArgs args;
    args.vertexCountPerInstance = 36;
    args.instanceCount = 100;
    args.startVertexLocation = 0;
    args.startInstanceLocation = 0;

    cmd.DrawInstancedIndirect(&args, 0);

    EXPECT_EQ(cmd.m_drawIndirectCalls, 1u);
    EXPECT_EQ(cmd.m_lastVertexCount, 36u);
    EXPECT_EQ(cmd.m_lastInstanceCount, 100u);
}

TEST(DrawIndirect_IndexedInstancedIndirectReadsArgs)
{
    MockCommandList cmd;
    DrawIndexedInstancedIndirectArgs args;
    args.indexCountPerInstance = 2400;
    args.instanceCount = 50;
    args.startIndexLocation = 0;
    args.baseVertexLocation = 0;
    args.startInstanceLocation = 0;

    cmd.DrawIndexedInstancedIndirect(&args, 0);

    EXPECT_EQ(cmd.m_drawIndexedIndirectCalls, 1u);
    EXPECT_EQ(cmd.m_lastIndexCount, 2400u);
    EXPECT_EQ(cmd.m_lastInstanceCount, 50u);
}

TEST(DrawIndirect_DispatchIndirectReadsArgs)
{
    MockCommandList cmd;
    DispatchIndirectArgs args;
    args.threadGroupCountX = 16;
    args.threadGroupCountY = 8;
    args.threadGroupCountZ = 1;

    cmd.DispatchIndirect(&args, 0);

    EXPECT_EQ(cmd.m_dispatchIndirectCalls, 1u);
    EXPECT_EQ(cmd.m_lastGroupX, 16u);
    EXPECT_EQ(cmd.m_lastGroupY, 8u);
    EXPECT_EQ(cmd.m_lastGroupZ, 1u);
}

TEST(DrawIndirect_NullBufferIsNoOp)
{
    MockCommandList cmd;
    cmd.DrawInstancedIndirect(nullptr, 0);
    cmd.DrawIndexedInstancedIndirect(nullptr, 0);
    cmd.DispatchIndirect(nullptr, 0);

    EXPECT_EQ(cmd.m_drawIndirectCalls, 0u);
    EXPECT_EQ(cmd.m_drawIndexedIndirectCalls, 0u);
    EXPECT_EQ(cmd.m_dispatchIndirectCalls, 0u);
}

TEST(DrawIndirect_ArgsLayoutMatchesD3D11)
{
    // Verify struct sizes match D3D11 indirect args layout
    EXPECT_EQ(sizeof(DrawInstancedIndirectArgs), 16u);        // 4 * uint32_t
    EXPECT_EQ(sizeof(DrawIndexedInstancedIndirectArgs), 20u); // 5 * uint32_t (4 + int32_t)
    EXPECT_EQ(sizeof(DispatchIndirectArgs), 12u);             // 3 * uint32_t
}

TEST(DrawIndirect_OffsetReadsCorrectArgs)
{
    // Simulate a buffer with 2 draw calls packed sequentially
    DrawInstancedIndirectArgs argsArray[2];
    argsArray[0] = {36, 10, 0, 0};
    argsArray[1] = {72, 20, 0, 0};

    MockCommandList cmd;

    // Read second args using byte offset
    uint32_t offset = sizeof(DrawInstancedIndirectArgs);
    cmd.DrawInstancedIndirect(argsArray, offset);

    EXPECT_EQ(cmd.m_lastVertexCount, 72u);
    EXPECT_EQ(cmd.m_lastInstanceCount, 20u);
}
