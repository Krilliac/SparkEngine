/**
 * @file TestAIDebugRendererReal.cpp
 * @brief Real-class tests for Spark::AI::AIDebugRenderer and DebugDrawOptions
 */

#include "TestFramework.h"
#include "Engine/AI/AIDebugRenderer.h"

using namespace Spark::AI;

// ============================================================================
// DebugDrawOptions defaults
// ============================================================================

TEST(AIDebugRendererReal_OptionsDefaults)
{
    DebugDrawOptions opts;
    EXPECT_TRUE(opts.drawNavMesh);
    EXPECT_FALSE(opts.drawNavMeshBounds);
    EXPECT_TRUE(opts.drawPaths);
    EXPECT_TRUE(opts.drawPerceptionCones);
    EXPECT_FALSE(opts.drawBehaviorTreeState);
    EXPECT_TRUE(opts.drawFormations);
    EXPECT_TRUE(opts.drawCoverPoints);
    EXPECT_FALSE(opts.drawTacticalPoints);
    EXPECT_NEAR(opts.navMeshOpacity, 0.3f, 0.001f);
    EXPECT_NEAR(opts.pathLineWidth, 2.0f, 0.001f);
}

TEST(AIDebugRendererReal_OptionsColors)
{
    DebugDrawOptions opts;
    EXPECT_EQ(opts.navMeshColor, 0x4000FF00u);
    EXPECT_EQ(opts.pathColor, 0xFFFFFF00u);
    EXPECT_EQ(opts.perceptionConeColor, 0x400000FFu);
    EXPECT_EQ(opts.coverPointColor, 0xFFFF0000u);
    EXPECT_EQ(opts.activeNodeColor, 0xFF00FF00u);
}

// ============================================================================
// AIDebugRenderer singleton and state
// ============================================================================

TEST(AIDebugRendererReal_SingletonStable)
{
    auto& a = AIDebugRenderer::GetInstance();
    auto& b = AIDebugRenderer::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(AIDebugRendererReal_DefaultDisabled)
{
    auto& renderer = AIDebugRenderer::GetInstance();
    // Default state: disabled
    EXPECT_FALSE(renderer.IsEnabled());
}

TEST(AIDebugRendererReal_EnableDisable)
{
    auto& renderer = AIDebugRenderer::GetInstance();
    renderer.SetEnabled(true);
    EXPECT_TRUE(renderer.IsEnabled());
    renderer.SetEnabled(false);
    EXPECT_FALSE(renderer.IsEnabled());
}

TEST(AIDebugRendererReal_SelectedAgent)
{
    auto& renderer = AIDebugRenderer::GetInstance();
    EXPECT_EQ(renderer.GetSelectedAgent(), 0u);
    renderer.SetSelectedAgent(42);
    EXPECT_EQ(renderer.GetSelectedAgent(), 42u);
    renderer.SetSelectedAgent(0);
    EXPECT_EQ(renderer.GetSelectedAgent(), 0u);
}

TEST(AIDebugRendererReal_SetGetOptions)
{
    auto& renderer = AIDebugRenderer::GetInstance();
    DebugDrawOptions custom;
    custom.drawNavMesh = false;
    custom.pathLineWidth = 5.0f;
    custom.navMeshOpacity = 0.8f;

    renderer.SetOptions(custom);
    const auto& opts = renderer.GetOptions();
    EXPECT_FALSE(opts.drawNavMesh);
    EXPECT_NEAR(opts.pathLineWidth, 5.0f, 0.001f);
    EXPECT_NEAR(opts.navMeshOpacity, 0.8f, 0.001f);

    // Restore defaults
    renderer.SetOptions(DebugDrawOptions{});
}

TEST(AIDebugRendererReal_OptionsMutability)
{
    DebugDrawOptions opts;
    opts.drawTacticalPoints = true;
    opts.coverPointColor = 0xFF00FFFFu;
    EXPECT_TRUE(opts.drawTacticalPoints);
    EXPECT_EQ(opts.coverPointColor, 0xFF00FFFFu);
}
