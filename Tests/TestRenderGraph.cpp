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

TEST(RenderGraph_ImportPreservesAllocationSignificantDescriptorFields)
{
    ::RenderTargetDesc descriptor;
    descriptor.width = 320;
    descriptor.height = 180;
    descriptor.arraySize = 6;
    descriptor.mipLevels = 4;
    descriptor.sampleCount = 1;
    descriptor.usage = RenderTargetUsage::DepthStencil | RenderTargetUsage::Array;
    descriptor.clearDepth = 0.25f;
    descriptor.clearStencil = 7;
    ::RenderTarget target(descriptor);

    RenderGraph graph("Test");
    const RenderGraphResource imported = graph.Import("ArrayTarget", &target);
    const auto& importedDescriptor = graph.GetResources()[imported.index].textureDesc;
    EXPECT_EQ(importedDescriptor.arraySize, 6u);
    EXPECT_EQ(importedDescriptor.mipLevels, 4u);
    EXPECT_EQ(static_cast<uint32_t>(importedDescriptor.usage), static_cast<uint32_t>(descriptor.usage));
    EXPECT_NEAR(importedDescriptor.clearDepth, 0.25f, 0.0001f);
    EXPECT_EQ(importedDescriptor.clearStencil, static_cast<uint8_t>(7));
}

// ============================================================================
// Adversarial dependency and aliasing cases
// ============================================================================

TEST(RenderGraph_ExactVersionDependencyDoesNotDependOnLaterOverwrite)
{
    RenderGraph graph("Versions");
    RenderGraphResource versionOne;

    auto& producer = graph.AddPass(
        "Producer", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder)
        {
            RenderGraphTextureDesc desc;
            versionOne = builder.Write(builder.Create("Versioned", desc));
        },
        [](const RenderGraphResourceRegistry&) {});
    producer.MarkSideEffects();

    auto& overwriter = graph.AddPass(
        "ProducesV2", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder) { builder.Write(versionOne); },
        [](const RenderGraphResourceRegistry&) {});
    overwriter.MarkSideEffects();

    // Registered after the overwrite, but it consumes the earlier physical
    // version and must execute before that allocation is overwritten.
    auto& consumer = graph.AddPass(
        "ConsumesV1", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder) { builder.Read(versionOne); },
        [](const RenderGraphResourceRegistry&) {});
    consumer.MarkSideEffects();

    graph.Compile();
    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), size_t(3));
    EXPECT_EQ(order[0], producer.GetIndex());
    EXPECT_EQ(order[1], consumer.GetIndex());
    EXPECT_EQ(order[2], overwriter.GetIndex());
}

TEST(RenderGraph_IndependentPassOrderIsStable)
{
    RenderGraph graph("StableOrder");
    for (int i = 0; i < 4; ++i)
    {
        auto& pass = graph.AddPass(
            "Independent" + std::to_string(i), RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
            [](const RenderGraphResourceRegistry&) {});
        pass.MarkSideEffects();
    }

    graph.Compile();
    const auto& order = graph.GetExecutionOrder();
    ASSERT_EQ(order.size(), size_t(4));
    for (uint32_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(order[i], i);
    }
}

TEST(RenderGraph_DependencyCycleIsRejected)
{
    RenderGraph graph("Cycle");
    const auto a = graph.Import("A", nullptr);
    const auto b = graph.Import("B", nullptr);
    const RenderGraphResource futureB{b.index, 1};
    RenderGraphResource a1;

    auto& first = graph.AddPass(
        "First", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder)
        {
            builder.Read(futureB);
            a1 = builder.Write(a);
        },
        [](const RenderGraphResourceRegistry&) {});
    first.MarkSideEffects();

    auto& second = graph.AddPass(
        "Second", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder)
        {
            builder.Read(a1);
            builder.Write(b);
        },
        [](const RenderGraphResourceRegistry&) {});
    second.MarkSideEffects();

    EXPECT_THROW(graph.Compile(), std::logic_error);
    EXPECT_FALSE(graph.IsCompiled());
    EXPECT_TRUE(graph.GetExecutionOrder().empty());
}

TEST(RenderGraph_UnknownResourceVersionIsRejected)
{
    RenderGraph graph("UnknownVersion");
    const auto imported = graph.Import("Imported", nullptr);
    RenderGraphResource unknown{imported.index, 7};
    auto& pass = graph.AddPass(
        "InvalidRead", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder) { builder.Read(unknown); },
        [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects();

    EXPECT_THROW(graph.Compile(), std::logic_error);
    EXPECT_FALSE(graph.IsCompiled());
}

TEST(RenderGraph_ThrowingPassSetupRollsBackAndInvalidates)
{
    RenderGraph graph("Test");
    RenderGraphResource existing;
    graph.AddPass(
        "Producer", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder)
        {
            existing = builder.Create("Existing", RenderGraphTextureDesc{});
        },
        [](const RenderGraphResourceRegistry&) {});
    graph.Compile();
    EXPECT_TRUE(graph.IsCompiled());

    const size_t originalPassCount = graph.GetPasses().size();
    const size_t originalResourceCount = graph.GetResources().size();
    const uint32_t originalRefCount = graph.GetResources()[existing.index].refCount;
    const size_t originalVersionCount = graph.GetResources()[existing.index].versionProducers.size();
    const bool originalVersionConflict = graph.GetResources()[existing.index].hasVersionConflict;

    EXPECT_THROW(
        graph.AddPass(
            "Throwing", RenderGraphPassType::Graphics,
            [&](RenderGraphBuilder& builder)
            {
                builder.Read(existing);
                builder.Write(existing);
                builder.Write(existing); // Same version producer marks a conflict until rollback.
                const auto temporary = builder.Create("Temporary", RenderGraphTextureDesc{});
                builder.Alias(temporary, existing);
                throw std::runtime_error("setup failed");
            },
            [](const RenderGraphResourceRegistry&) {}),
        std::runtime_error);

    EXPECT_FALSE(graph.IsCompiled());
    EXPECT_EQ(graph.GetPasses().size(), originalPassCount);
    EXPECT_EQ(graph.GetResources().size(), originalResourceCount);
    EXPECT_EQ(graph.GetResources()[existing.index].refCount, originalRefCount);
    EXPECT_EQ(graph.GetResources()[existing.index].versionProducers.size(), originalVersionCount);
    EXPECT_EQ(graph.GetResources()[existing.index].hasVersionConflict, originalVersionConflict);
    EXPECT_NO_THROW(graph.Compile());
}

TEST(RenderGraph_ExecuteRejectsUncompiledGraph)
{
    RenderGraph graph("NotCompiled");
    EXPECT_THROW(graph.Execute(), std::logic_error);
}

TEST(RenderGraph_FailedRecompileClearsPriorSchedule)
{
    RenderGraph graph("FailedRecompile");
    auto& valid = graph.AddPass(
        "Valid", RenderGraphPassType::Graphics, [](RenderGraphBuilder&) {},
        [](const RenderGraphResourceRegistry&) {});
    valid.MarkSideEffects();
    graph.Compile();
    ASSERT_EQ(graph.GetExecutionOrder().size(), size_t(1));

    auto& invalid = graph.AddPass(
        "Invalid", RenderGraphPassType::Graphics,
        [](RenderGraphBuilder& builder)
        { builder.Read(RenderGraphResource{RenderGraphResource::INVALID_INDEX, 0}); },
        [](const RenderGraphResourceRegistry&) {});
    invalid.MarkSideEffects();

    EXPECT_THROW(graph.Compile(), std::logic_error);
    EXPECT_FALSE(graph.IsCompiled());
    EXPECT_TRUE(graph.GetExecutionOrder().empty());
    EXPECT_THROW(graph.Execute(), std::logic_error);
}

TEST(RenderGraph_AliasGroupMembersNeverOverlap)
{
    RenderGraph graph("AliasGroups");
    RenderGraphResource a;
    RenderGraphResource b;
    RenderGraphResource c;
    RenderGraphTextureDesc desc;
    desc.width = 64;
    desc.height = 64;

    auto& passA = graph.AddPass(
        "A", RenderGraphPassType::Graphics, [&](RenderGraphBuilder& builder) { a = builder.Create("A", desc); },
        [](const RenderGraphResourceRegistry&) {});
    passA.MarkSideEffects();
    auto& passB = graph.AddPass(
        "BStart", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder) { b = builder.Create("B", desc); },
        [](const RenderGraphResourceRegistry&) {});
    passB.MarkSideEffects();
    auto& passC = graph.AddPass(
        "C", RenderGraphPassType::Graphics, [&](RenderGraphBuilder& builder) { c = builder.Create("C", desc); },
        [](const RenderGraphResourceRegistry&) {});
    passC.MarkSideEffects();
    auto& useB = graph.AddPass(
        "BEnd", RenderGraphPassType::Graphics, [&](RenderGraphBuilder& builder) { builder.Read(b); },
        [](const RenderGraphResourceRegistry&) {});
    useB.MarkSideEffects();

    graph.Compile();
    const auto& resources = graph.GetResources();
    EXPECT_EQ(resources[b.index].aliasTarget, a.index);
    EXPECT_EQ(resources[c.index].aliasTarget, RenderGraphResource::INVALID_INDEX);
    EXPECT_EQ(graph.GetStats().aliasedResources, uint32_t(1));
}

TEST(RenderGraph_AliasingRequiresExactAllocationCompatibility)
{
    RenderGraph graph("AliasCompatibility");
    RenderGraphTextureDesc wide;
    wide.width = 64;
    wide.height = 64;
    RenderGraphTextureDesc narrow = wide;
    narrow.width = 128;
    narrow.height = 32; // Same byte count, incompatible texture shape.

    auto& first = graph.AddPass(
        "Wide", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder) { builder.Create("Wide", wide); },
        [](const RenderGraphResourceRegistry&) {});
    first.MarkSideEffects();
    auto& second = graph.AddPass(
        "Narrow", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder) { builder.Create("Narrow", narrow); },
        [](const RenderGraphResourceRegistry&) {});
    second.MarkSideEffects();

    graph.Compile();
    EXPECT_EQ(graph.GetStats().aliasedResources, uint32_t(0));
}

TEST(RenderGraph_OverlappingExplicitAliasIsRejected)
{
    RenderGraph graph("UnsafeAlias");
    auto& pass = graph.AddPass(
        "BothLive", RenderGraphPassType::Graphics,
        [&](RenderGraphBuilder& builder)
        {
            RenderGraphTextureDesc desc;
            const auto a = builder.Create("A", desc);
            const auto b = builder.Create("B", desc);
            builder.Alias(a, b);
        },
        [](const RenderGraphResourceRegistry&) {});
    pass.MarkSideEffects();

    EXPECT_THROW(graph.Compile(), std::logic_error);
    EXPECT_FALSE(graph.IsCompiled());
}
