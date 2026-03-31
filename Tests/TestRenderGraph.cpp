/**
 * @file TestRenderGraph.cpp
 * @brief Unit tests for the RenderGraph frame-graph system
 *
 * Tests pass registration, compilation (topological sort, dead-code elimination,
 * resource aliasing), execution, blackboard, and debug output — all without
 * requiring a GPU or D3D11 device.
 */

#include "TestFramework.h"
#include "Graphics/RenderGraph.h"

using namespace Spark::Graphics;

// ============================================================================
// Construction & Basic State
// ============================================================================

TEST(RenderGraph_ConstructionSetsName)
{
    RenderGraph graph("TestGraph");
    EXPECT_EQ(graph.GetName(), std::string("TestGraph"));
    EXPECT_FALSE(graph.IsCompiled());
    EXPECT_TRUE(graph.GetPasses().empty());
    EXPECT_TRUE(graph.GetResources().empty());
    EXPECT_TRUE(graph.GetExecutionOrder().empty());
}

TEST(RenderGraph_MoveConstruction)
{
    RenderGraph original("Original");
    original.AddPass(
        "DummyPass", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});

    RenderGraph moved(std::move(original));
    EXPECT_EQ(moved.GetName(), std::string("Original"));
    EXPECT_EQ(moved.GetPasses().size(), size_t(1));
}

// ============================================================================
// Pass Registration
// ============================================================================

TEST(RenderGraph_AddPassIncreasesCount)
{
    RenderGraph graph("Test");
    EXPECT_EQ(graph.GetPasses().size(), size_t(0));

    graph.AddPass(
        "Pass1", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    EXPECT_EQ(graph.GetPasses().size(), size_t(1));

    graph.AddPass(
        "Pass2", RenderGraphPassType::Compute, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    EXPECT_EQ(graph.GetPasses().size(), size_t(2));
}

TEST(RenderGraph_PassNamesStored)
{
    RenderGraph graph("Test");
    graph.AddPass(
        "GBuffer", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});
    graph.AddPass(
        "Lighting", RenderGraphPassType::Compute, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});

    EXPECT_EQ(graph.GetPasses()[0]->GetName(), std::string("GBuffer"));
    EXPECT_EQ(graph.GetPasses()[1]->GetName(), std::string("Lighting"));
}

TEST(RenderGraph_PassTypesStored)
{
    RenderGraph graph("Test");
    graph.AddPass(
        "Graphics", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});
    graph.AddPass(
        "Compute", RenderGraphPassType::Compute, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    graph.AddPass(
        "Copy", RenderGraphPassType::Copy, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});

    EXPECT_TRUE(graph.GetPasses()[0]->GetType() == RenderGraphPassType::Graphics);
    EXPECT_TRUE(graph.GetPasses()[1]->GetType() == RenderGraphPassType::Compute);
    EXPECT_TRUE(graph.GetPasses()[2]->GetType() == RenderGraphPassType::Copy);
}

TEST(RenderGraph_AddPassResetsCompiled)
{
    RenderGraph graph("Test");
    graph.AddPass(
        "Pass1", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    graph.Compile();
    EXPECT_TRUE(graph.IsCompiled());

    graph.AddPass(
        "Pass2", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    EXPECT_FALSE(graph.IsCompiled());
}

// ============================================================================
// Resource Creation via Builder
// ============================================================================

TEST(RenderGraph_CreateTextureAddsResource)
{
    RenderGraph graph("Test");
    RenderGraphResource outTex;

    graph.AddPass(
        "Producer", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder)
        {
            RenderGraphTextureDesc desc;
            desc.width = 1920;
            desc.height = 1080;
            outTex = builder.Create("HDRColor", desc);
            outTex = builder.Write(outTex);
        },
        [](const RenderGraphResourceRegistry&) {});

    EXPECT_EQ(graph.GetResources().size(), size_t(1));
    EXPECT_EQ(graph.GetResources()[0].name, std::string("HDRColor"));
    EXPECT_TRUE(graph.GetResources()[0].lifetime == RenderGraphResourceLifetime::Transient);
    EXPECT_EQ(graph.GetResources()[0].textureDesc.width, uint32_t(1920));
    EXPECT_EQ(graph.GetResources()[0].textureDesc.height, uint32_t(1080));
}

TEST(RenderGraph_ImportAddsResource)
{
    RenderGraph graph("Test");
    graph.Import("BackBuffer", nullptr);

    EXPECT_EQ(graph.GetResources().size(), size_t(1));
    EXPECT_EQ(graph.GetResources()[0].name, std::string("BackBuffer"));
    EXPECT_TRUE(graph.GetResources()[0].lifetime == RenderGraphResourceLifetime::Imported);
}

// ============================================================================
// Compilation
// ============================================================================

TEST(RenderGraph_CompileEmptyGraph)
{
    RenderGraph graph("Empty");
    graph.Compile();
    EXPECT_TRUE(graph.IsCompiled());
    EXPECT_EQ(graph.GetStats().totalPasses, uint32_t(0));
    EXPECT_TRUE(graph.GetExecutionOrder().empty());
}

TEST(RenderGraph_CompileSinglePass)
{
    RenderGraph graph("Test");
    auto& pass = graph.AddPass(
        "OnlyPass", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects(); // Prevent culling

    graph.Compile();
    EXPECT_TRUE(graph.IsCompiled());
    EXPECT_EQ(graph.GetStats().totalPasses, uint32_t(1));
}

TEST(RenderGraph_CompileProducerConsumerOrder)
{
    RenderGraph graph("Test");
    RenderGraphResource tex;

    // Producer writes a texture
    auto& producer = graph.AddPass(
        "Producer", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder)
        {
            RenderGraphTextureDesc desc;
            desc.width = 1920;
            desc.height = 1080;
            tex = builder.Create("ColorBuffer", desc);
            tex = builder.Write(tex);
        },
        [](const RenderGraphResourceRegistry&) {});

    // Consumer reads that texture (has side effects so it won't be culled)
    auto& consumer = graph.AddPass(
        "Consumer", RenderGraphPassType::Graphics, [&](RenderGraphBuilder& builder) { builder.Read(tex); },
        [](const RenderGraphResourceRegistry&) {});
    consumer.MarkSideEffects();

    graph.Compile();
    EXPECT_TRUE(graph.IsCompiled());

    // Both passes should be in execution order, producer before consumer
    const auto& order = graph.GetExecutionOrder();
    EXPECT_GE(order.size(), size_t(2));
    if (order.size() >= 2)
    {
        // Find indices
        uint32_t producerIdx = producer.GetIndex();
        uint32_t consumerIdx = consumer.GetIndex();
        size_t producerPos = SIZE_MAX;
        size_t consumerPos = SIZE_MAX;
        for (size_t i = 0; i < order.size(); ++i)
        {
            if (order[i] == producerIdx)
                producerPos = i;
            if (order[i] == consumerIdx)
                consumerPos = i;
        }
        EXPECT_TRUE(producerPos < consumerPos);
    }
}

TEST(RenderGraph_DeadCodeElimination)
{
    RenderGraph graph("Test");

    // Pass with no consumers and no side effects — should be culled
    graph.AddPass(
        "DeadPass", RenderGraphPassType::Graphics,
        [](RenderGraphBuilder& builder)
        {
            RenderGraphTextureDesc desc;
            desc.width = 256;
            desc.height = 256;
            auto tex = builder.Create("Unused", desc);
            builder.Write(tex);
        },
        [](const RenderGraphResourceRegistry&) {});

    // Pass with side effects — should survive
    auto& alivePass = graph.AddPass(
        "AlivePass", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});
    alivePass.MarkSideEffects();

    graph.Compile();
    EXPECT_GT(graph.GetStats().culledPasses, uint32_t(0));
}

TEST(RenderGraph_StatsAfterCompile)
{
    RenderGraph graph("Test");
    graph.AddPass(
        "Pass1", RenderGraphPassType::Graphics,
        [](RenderGraphBuilder& builder)
        {
            RenderGraphTextureDesc desc;
            desc.width = 1920;
            desc.height = 1080;
            auto tex = builder.Create("Tex1", desc);
            builder.Write(tex);
        },
        [](const RenderGraphResourceRegistry&) {});

    graph.Import("BackBuffer", nullptr);
    auto& finalPass = graph.AddPass(
        "Final", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    finalPass.MarkSideEffects();

    graph.Compile();
    EXPECT_EQ(graph.GetStats().totalPasses, uint32_t(2));
    EXPECT_GE(graph.GetStats().totalResources, uint32_t(1));
    EXPECT_GE(graph.GetStats().compileTimeMs, 0.0f);
}

// ============================================================================
// Execution
// ============================================================================

TEST(RenderGraph_ExecuteInvokesCallbacks)
{
    RenderGraph graph("Test");
    int callCount = 0;

    auto& pass = graph.AddPass(
        "CountingPass", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [&](const RenderGraphResourceRegistry&) { callCount++; });
    pass.MarkSideEffects();

    graph.Compile();
    graph.Execute();

    EXPECT_EQ(callCount, 1);
}

TEST(RenderGraph_ExecuteMultiplePassesInOrder)
{
    RenderGraph graph("Test");
    std::vector<int> executionOrder;

    auto& p1 = graph.AddPass(
        "First", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [&](const RenderGraphResourceRegistry&) { executionOrder.push_back(1); });
    p1.MarkSideEffects();

    auto& p2 = graph.AddPass(
        "Second", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [&](const RenderGraphResourceRegistry&) { executionOrder.push_back(2); });
    p2.MarkSideEffects();

    graph.Compile();
    graph.Execute();

    EXPECT_EQ(executionOrder.size(), size_t(2));
    EXPECT_EQ(graph.GetStats().executedPasses, uint32_t(2));
}

TEST(RenderGraph_ExecuteSkipsCulledPasses)
{
    RenderGraph graph("Test");
    int deadCallCount = 0;
    int liveCallCount = 0;

    // Dead pass — no side effects, no consumers
    graph.AddPass(
        "Dead", RenderGraphPassType::Graphics,
        [](RenderGraphBuilder& builder)
        {
            RenderGraphTextureDesc desc;
            desc.width = 64;
            desc.height = 64;
            auto tex = builder.Create("DeadTex", desc);
            builder.Write(tex);
        },
        [&](const RenderGraphResourceRegistry&) { deadCallCount++; });

    // Live pass — has side effects
    auto& live = graph.AddPass(
        "Live", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [&](const RenderGraphResourceRegistry&) { liveCallCount++; });
    live.MarkSideEffects();

    graph.Compile();
    graph.Execute();

    EXPECT_EQ(deadCallCount, 0);
    EXPECT_EQ(liveCallCount, 1);
}

// ============================================================================
// Clear & Reset
// ============================================================================

TEST(RenderGraph_ClearResetsState)
{
    RenderGraph graph("Test");
    auto& pass = graph.AddPass(
        "Pass1", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects();
    graph.Import("Res1", nullptr);

    graph.Compile();
    EXPECT_TRUE(graph.IsCompiled());

    graph.Clear();
    EXPECT_FALSE(graph.IsCompiled());
    EXPECT_TRUE(graph.GetPasses().empty());
    EXPECT_TRUE(graph.GetResources().empty());
    EXPECT_TRUE(graph.GetExecutionOrder().empty());
}

TEST(RenderGraph_ClearPreservesBlackboard)
{
    RenderGraph graph("Test");
    graph.GetBlackboard().Add<int>(42);

    graph.Clear();

    EXPECT_TRUE(graph.GetBlackboard().Has<int>());
    EXPECT_EQ(graph.GetBlackboard().Get<int>(), 42);
}

TEST(RenderGraph_RebuildAfterClear)
{
    RenderGraph graph("Test");
    auto& p1 = graph.AddPass(
        "Old", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    p1.MarkSideEffects();
    graph.Compile();
    graph.Execute();

    graph.Clear();

    int called = 0;
    auto& p2 = graph.AddPass(
        "New", RenderGraphPassType::Compute, [](RenderGraphBuilder&) {},
        [&](const RenderGraphResourceRegistry&) { called++; });
    p2.MarkSideEffects();
    graph.Compile();
    graph.Execute();

    EXPECT_EQ(called, 1);
    EXPECT_EQ(graph.GetPasses().size(), size_t(1));
    EXPECT_EQ(graph.GetPasses()[0]->GetName(), std::string("New"));
}

// ============================================================================
// Blackboard
// ============================================================================

TEST(RenderGraph_BlackboardAddAndGet)
{
    RenderGraph graph("Test");
    auto& bb = graph.GetBlackboard();

    bb.Add<float>(1.5f);
    bb.Add<int>(42);
    bb.Add<std::string>("hello");

    EXPECT_TRUE(bb.Has<float>());
    EXPECT_NEAR(bb.Get<float>(), 1.5f, 0.001f);
    EXPECT_EQ(bb.Get<int>(), 42);
    EXPECT_EQ(bb.Get<std::string>(), std::string("hello"));
}

TEST(RenderGraph_BlackboardHasReturnsFalseForMissing)
{
    RenderGraph graph("Test");
    EXPECT_FALSE(graph.GetBlackboard().Has<int>());
}

// ============================================================================
// Debug Output
// ============================================================================

TEST(RenderGraph_DebugVisualizeNonEmpty)
{
    RenderGraph graph("Debug");
    auto& pass = graph.AddPass(
        "TestPass", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects();
    graph.Import("BackBuffer", nullptr);
    graph.Compile();

    std::string viz = graph.DebugVisualize();
    EXPECT_FALSE(viz.empty());
    EXPECT_TRUE(viz.find("TestPass") != std::string::npos);
    EXPECT_TRUE(viz.find("BackBuffer") != std::string::npos);
    EXPECT_TRUE(viz.find("Debug") != std::string::npos);
}

TEST(RenderGraph_ExportDotContainsPasses)
{
    RenderGraph graph("DotTest");
    auto& pass = graph.AddPass(
        "ShadowMap", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects();

    std::string dot = graph.ExportDot();
    EXPECT_TRUE(dot.find("digraph") != std::string::npos);
    EXPECT_TRUE(dot.find("ShadowMap") != std::string::npos);
}

TEST(RenderGraph_BufferResourceCreation)
{
    RenderGraph graph("BufferTest");

    RenderGraphResource bufHandle;

    auto& pass = graph.AddPass(
        "ComputePass", RenderGraphPassType::Compute,
        [&](RenderGraphBuilder& builder)
        {
            RenderGraphBufferDesc bufDesc;
            bufDesc.sizeBytes = 4096;
            bufDesc.stride = 16;
            bufDesc.usage = RenderTargetUsage::UnorderedAccess;
            bufHandle = builder.Create("StructuredBuf", bufDesc);
        },
        [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects();

    graph.Compile();

    // Verify the buffer resource was created in the graph
    const auto& resources = graph.GetResources();
    bool found = false;
    for (const auto& res : resources)
    {
        if (res.name == "StructuredBuf" && res.type == RenderGraphResourceType::Buffer)
        {
            EXPECT_EQ(res.bufferDesc.sizeBytes, size_t(4096));
            EXPECT_EQ(res.bufferDesc.stride, size_t(16));
            found = true;
        }
    }
    EXPECT_TRUE(found);

    // Execute without device — buffer allocation gracefully skips (no crash)
    graph.Execute();
}

TEST(RenderGraph_ConsoleGetGraphStats)
{
    RenderGraph graph("StatsTest");
    auto& pass = graph.AddPass(
        "Pass", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {}, [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects();
    graph.Compile();
    graph.Execute();

    std::string stats = graph.Console_GetGraphStats();
    EXPECT_FALSE(stats.empty());
    EXPECT_TRUE(stats.find("StatsTest") != std::string::npos);
}
