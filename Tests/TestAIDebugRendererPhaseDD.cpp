/**
 * @file TestAIDebugRendererPhaseDD.cpp
 * @brief Phase DD Theme 3D tests for Spark::AI::AIDebugRenderer
 *
 * The existing Tests/TestAIDebugRenderer.cpp is yet another fake-
 * coverage test file (local TestAIDbg namespace with its own
 * XMFLOAT3/DebugDrawOptions reimpls). Phase DD ships real-class
 * tests against the genuine singleton and wires Initialize /
 * Shutdown into GameplayLifecycleShared.cpp so the debug overlay
 * is reachable from any AI or ImGui menu code.
 *
 * Coverage:
 *   - Singleton accessor stability
 *   - Initialize + Shutdown lifecycle
 *   - Default enabled state is false
 *   - SetEnabled / IsEnabled round-trip
 *   - SetOptions / GetOptions round-trip for every bool + color field
 *   - SetSelectedAgent / GetSelectedAgent round-trip
 *   - Console_GetStatus returns a non-empty string
 *   - Shutdown is idempotent
 *   - Update is a no-op when disabled (must not crash)
 */

#include "TestFramework.h"
#include "Engine/AI/AIDebugRenderer.h"

TEST(AIDebugRendererPhaseDD_SingletonReturnsSameInstance)
{
    auto& a = Spark::AI::AIDebugRenderer::GetInstance();
    auto& b = Spark::AI::AIDebugRenderer::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(AIDebugRendererPhaseDD_InitializeShutdownLifecycle)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Shutdown();
    r.Initialize();
    // Re-initialising is safe.
    r.Initialize();
    r.Shutdown();
    r.Shutdown();
    // Restore for other tests.
    r.Initialize();
}

TEST(AIDebugRendererPhaseDD_DefaultDisabled)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Shutdown();
    r.Initialize();
    // Freshly initialised: debug overlay must be off.
    EXPECT_FALSE(r.IsEnabled());
}

TEST(AIDebugRendererPhaseDD_SetEnabledToggles)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Initialize();

    r.SetEnabled(true);
    EXPECT_TRUE(r.IsEnabled());

    r.SetEnabled(false);
    EXPECT_FALSE(r.IsEnabled());
}

TEST(AIDebugRendererPhaseDD_OptionsRoundTrip)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Initialize();

    Spark::AI::DebugDrawOptions opts;
    opts.drawNavMesh = false;
    opts.drawPaths = false;
    opts.drawPerceptionCones = true;
    opts.drawBehaviorTreeState = true;
    opts.drawFormations = false;
    opts.drawCoverPoints = true;
    opts.drawTacticalPoints = true;
    opts.navMeshOpacity = 0.75f;
    opts.pathLineWidth = 4.0f;
    opts.navMeshColor = 0xDEADBEEF;
    opts.pathColor = 0xCAFEBABE;
    r.SetOptions(opts);

    const auto& retrieved = r.GetOptions();
    EXPECT_FALSE(retrieved.drawNavMesh);
    EXPECT_FALSE(retrieved.drawPaths);
    EXPECT_TRUE(retrieved.drawPerceptionCones);
    EXPECT_TRUE(retrieved.drawBehaviorTreeState);
    EXPECT_FALSE(retrieved.drawFormations);
    EXPECT_TRUE(retrieved.drawCoverPoints);
    EXPECT_TRUE(retrieved.drawTacticalPoints);
    EXPECT_NEAR(retrieved.navMeshOpacity, 0.75f, 0.001f);
    EXPECT_NEAR(retrieved.pathLineWidth, 4.0f, 0.001f);
    EXPECT_EQ(retrieved.navMeshColor, static_cast<uint32_t>(0xDEADBEEF));
    EXPECT_EQ(retrieved.pathColor, static_cast<uint32_t>(0xCAFEBABE));
}

TEST(AIDebugRendererPhaseDD_SelectedAgentRoundTrip)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Initialize();

    r.SetSelectedAgent(42);
    EXPECT_EQ(r.GetSelectedAgent(), static_cast<uint32_t>(42));

    r.SetSelectedAgent(0);
    EXPECT_EQ(r.GetSelectedAgent(), static_cast<uint32_t>(0));
}

TEST(AIDebugRendererPhaseDD_ConsoleStatusReturnsString)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Initialize();
    r.SetEnabled(true);
    const auto status = r.Console_GetStatus();
    EXPECT_TRUE(!status.empty());
}

TEST(AIDebugRendererPhaseDD_UpdateWhenDisabledIsSafe)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Initialize();
    r.SetEnabled(false);
    // Must not crash when there's nothing to draw.
    r.Update(0.016f);
    r.Update(0.016f);
    EXPECT_FALSE(r.IsEnabled());
}

TEST(AIDebugRendererPhaseDD_UpdateWhenEnabledIsSafeWithNoData)
{
    auto& r = Spark::AI::AIDebugRenderer::GetInstance();
    r.Initialize();
    r.SetEnabled(true);
    // Enabled with no agents / navmeshes / paths — Update must still
    // not crash. Cleanup afterwards.
    r.Update(0.016f);
    r.SetEnabled(false);
}
