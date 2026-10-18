/**
 * @file TestGraphicsEngineLinuxPassTruthReal.cpp
 * @brief RHI-240: the Linux render passes report only work they actually record.
 *
 * The Linux (RHI-bridge) GraphicsEngine passes used to issue `Draw(3, 0)`
 * full-screen triangles with no pipeline, shader, inputs or targets bound
 * (lighting resolve, Bloom, SSAO, tone mapping, TAA, motion blur) and to bump
 * `RenderStatistics::drawCalls` / `postProcessPasses` for passes that did not
 * exist. These tests drive the production Linux passes on NullRHI and assert
 * that nothing is recorded or reported when nothing is bound.
 *
 * NullCommandList counts every draw and dispatch call it receives, so the
 * command-list counters are the ground truth for "work recorded".
 */

#include "Core/Platform.h"
#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS

// The render sub-passes are private (only RenderPipeline, which is compiled on
// Windows alone, is a friend). Every header GraphicsEngine.h pulls in is
// included first with normal access, so the access override below reaches
// only the GraphicsEngine class declaration in this test translation unit.
#include "Game/GameObject.h"
#include "Graphics/GraphicsEngineRHI.h"
#include "Graphics/RHI/NullRHIDevice.h"
#include "Utils/Assert.h"
#include "Core/framework.h"
#include "Graphics/GraphicsEngineTypes.h"
#include "Graphics/Shader.h"
#include "Graphics/DrawSortKey.h"
#include "Graphics/PipelineStateCache.h"
#include "Graphics/RenderTargetPool.h"
#include "Graphics/BVHAccelerator.h"
#include "Graphics/GPUSceneBuffer.h"
#include "Graphics/ConstantBufferRing.h"
#include "Graphics/GPUDebugMarkers.h"
#include "Graphics/MakeDesc.h"
#include "Graphics/TerrainRenderer.h"
#include "Graphics/GPUTimestampQuery.h"
#include "Graphics/GraphicsBenchmarkStats.h"
#include "Graphics/DenoiserInterface.h"
#include "Graphics/FastNoise2SIMD.h"
#include "Graphics/VoxelConeTracing.h"
#include "Graphics/RHI/RHIBridge.h"
#include "Graphics/RenderPipeline.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// clang-format off
#define private public
#include "Graphics/GraphicsEngine.h"
#undef private
// clang-format on

namespace
{
    // Every effect whose pass used to be faked on Linux is switched on, so a
    // regression in any of them shows up as a recorded draw or a counted pass.
    void EnableEveryFakedEffect(GraphicsEngine& engine)
    {
        GraphicsSettings settings = engine.GetGraphicsSettings();
        settings.bloom = true;
        settings.ssao = true;
        settings.taa = true;
        settings.motionBlur = true;
        engine.SetGraphicsSettings(settings);
        engine.SetHDREnabled(true);
    }

    // GameObject is abstract only over its hit callbacks, which this test
    // never triggers; everything else is the production class.
    class MeshlessObject final : public GameObject
    {
      public:
        void OnHit(GameObject* /*target*/) override {}
        void OnHitWorld(const DirectX::XMFLOAT3& /*hitPoint*/, const DirectX::XMFLOAT3& /*normal*/) override {}
    };

    Spark::RHI::NullCommandList* HeadlessCommandList()
    {
        return dynamic_cast<Spark::RHI::NullCommandList*>(Spark::Graphics::Detail::GetRHI().bridge.GetCommandList());
    }
} // namespace

TEST(GraphicsLinuxPassTruth_RenderPassesRecordNoUnboundDraws)
{
    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.Initialize(nullptr)));
    EnableEveryFakedEffect(engine);

    MeshlessObject object;
    object.SetActive(true);
    object.SetVisible(true);
    const std::vector<GameObject*> objects{&object};

    engine.BeginFrame();
    Spark::RHI::NullCommandList* cmd = HeadlessCommandList();
    ASSERT_TRUE(cmd != nullptr);
    const uint32_t drawsBefore = cmd->GetDrawCallCount();
    const uint32_t dispatchesBefore = cmd->GetDispatchCount();

    // Every Linux pipeline entry point and sub-pass that used to record an
    // unbound full-screen draw or bump a counter.
    const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
    engine.RenderForward(identity, identity, objects);
    engine.RenderDeferred(identity, identity, objects);
    engine.RenderForwardPlus(identity, identity, objects);
    engine.FillGBuffer(objects, identity, identity);
    engine.LightingPass(identity, identity);
    engine.RenderTemporalEffects();
    engine.RenderPostProcessing();

    EXPECT_EQ(cmd->GetDrawCallCount(), drawsBefore);
    EXPECT_EQ(cmd->GetDispatchCount(), dispatchesBefore);
    EXPECT_EQ(engine.GetStatistics().drawCalls, 0u);
    EXPECT_EQ(engine.GetStatistics().postProcessPasses, 0u);

    engine.EndFrame();

    // EndFrame folds in the backend's own counters; NullRHI draws nothing.
    EXPECT_EQ(engine.GetStatistics().drawCalls, 0u);
    EXPECT_EQ(engine.GetStatistics().postProcessPasses, 0u);

    engine.Shutdown();
}

TEST(GraphicsLinuxPassTruth_RenderSceneDoesNotCountObjectsThatRecordNothing)
{
    GraphicsEngine engine;
    ASSERT_TRUE(SUCCEEDED(engine.Initialize(nullptr)));

    // An active, visible object without a mesh reaches GameObject::Render and
    // returns without recording anything. It is visible, but it is not a draw.
    MeshlessObject object;
    object.SetActive(true);
    object.SetVisible(true);
    const std::vector<GameObject*> objects{&object};

    engine.BeginFrame();
    Spark::RHI::NullCommandList* cmd = HeadlessCommandList();
    ASSERT_TRUE(cmd != nullptr);
    const uint32_t drawsBefore = cmd->GetDrawCallCount();

    const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
    engine.RenderScene(identity, identity, objects);

    EXPECT_EQ(cmd->GetDrawCallCount(), drawsBefore);
    EXPECT_EQ(engine.GetStatistics().visibleObjects, 1u);
    EXPECT_EQ(engine.GetStatistics().drawCalls, 0u);

    engine.EndFrame();
    EXPECT_EQ(engine.GetStatistics().drawCalls, 0u);

    engine.Shutdown();
}

#endif // !SPARK_PLATFORM_WINDOWS
