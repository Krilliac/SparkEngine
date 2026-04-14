/**
 * @file TestAIIntegratedSystem.cpp
 * @brief Real-class tests for Spark::AI::AIIntegratedSystem after its
 *        2026-04-14 wire-in into GameplayLifecycleShared.cpp.
 *
 * The system was previously orphaned (defined in AIIntegration.h but never
 * instantiated). It is now exposed as a singleton and ticked from the
 * lifecycle every frame, with the inner heavy AISystem.Update gated behind
 * AIIntegrationConfig::runCoreAISystem (default false) so behavior trees are
 * not double-ticked alongside Spark::ECS::AIUpdateSystem.
 *
 * These tests cover the wire-relevant surface area: lifecycle, config,
 * subsystem accessors, status output, and Update() against a real World on
 * an empty registry (just to prove the additive path doesn't crash without
 * agents).
 */

#include "TestFramework.h"

#include "Engine/AI/AIIntegration.h"
#include "Engine/ECS/Components.h"

using namespace Spark::AI;

namespace
{
    // Each test uses a freshly Shutdown'd singleton so we don't carry state
    // across tests in undefined order.
    void ResetSingleton()
    {
        AIIntegratedSystem::GetInstance().Shutdown();
    }
} // namespace

TEST(AIIntegrated_SingletonLiveness)
{
    auto& a = AIIntegratedSystem::GetInstance();
    auto& b = AIIntegratedSystem::GetInstance();
    EXPECT_EQ(&a, &b);
}

TEST(AIIntegrated_InitializeFromDefault)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    EXPECT_FALSE(sys.IsInitialized());
    sys.Initialize();
    EXPECT_TRUE(sys.IsInitialized());
    sys.Shutdown();
    EXPECT_FALSE(sys.IsInitialized());
}

TEST(AIIntegrated_ConfigDefaultDoesNotRunCoreAISystem)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    sys.Initialize();
    // Default config must keep the heavy inner pipeline OFF so we don't
    // double-tick behavior trees alongside ECS::AIUpdateSystem.
    EXPECT_FALSE(sys.RunCoreAISystem());
    sys.Shutdown();
}

TEST(AIIntegrated_SetRunCoreAISystemToggle)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    sys.Initialize();
    sys.SetRunCoreAISystem(true);
    EXPECT_TRUE(sys.RunCoreAISystem());
    sys.SetRunCoreAISystem(false);
    EXPECT_FALSE(sys.RunCoreAISystem());
    sys.Shutdown();
}

TEST(AIIntegrated_SubsystemAccessorsAfterDefaultInit)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    sys.Initialize();
    // Default config enables all three subsystems, so each accessor must
    // return non-null after Initialize.
    EXPECT_TRUE(sys.GetParallelPerception() != nullptr);
    EXPECT_TRUE(sys.GetBudgetLimiter() != nullptr);
    EXPECT_TRUE(sys.GetObstacleManager() != nullptr);
    sys.Shutdown();
    // Shutdown must release them.
    EXPECT_TRUE(sys.GetParallelPerception() == nullptr);
    EXPECT_TRUE(sys.GetBudgetLimiter() == nullptr);
    EXPECT_TRUE(sys.GetObstacleManager() == nullptr);
}

TEST(AIIntegrated_SubsystemAccessorsHonorOptOuts)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    AIIntegrationConfig cfg;
    cfg.enableParallelPerception = false;
    cfg.enableBudgetLimiter = false;
    cfg.enableNavMeshObstacles = false;
    sys.Initialize(cfg);
    EXPECT_TRUE(sys.GetParallelPerception() == nullptr);
    EXPECT_TRUE(sys.GetBudgetLimiter() == nullptr);
    EXPECT_TRUE(sys.GetObstacleManager() == nullptr);
    EXPECT_TRUE(sys.IsInitialized());
    sys.Shutdown();
}

TEST(AIIntegrated_UpdateBeforeInitializeIsNoOp)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    World world;
    // Should be a no-op (and definitely not crash) when not initialized.
    EXPECT_NO_THROW(sys.Update(world, 0.016f));
    EXPECT_FALSE(sys.IsInitialized());
}

TEST(AIIntegrated_UpdateOnEmptyWorldSucceeds)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    sys.Initialize();
    World world;
    // Empty world -- no AI agents -- must not crash and must not invoke the
    // inner heavy pipeline (default cfg has runCoreAISystem=false).
    EXPECT_NO_THROW(sys.Update(world, 0.016f));
    EXPECT_NO_THROW(sys.Update(world, 0.016f));
    EXPECT_FALSE(sys.RunCoreAISystem());
    sys.Shutdown();
}

TEST(AIIntegrated_StatusBeforeInitialize)
{
    ResetSingleton();
    auto status = AIIntegratedSystem::GetInstance().Console_GetStatus();
    EXPECT_TRUE(status.find("Initialized:") != std::string::npos);
    EXPECT_TRUE(status.find("NO") != std::string::npos);
}

TEST(AIIntegrated_StatusAfterInitialize)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    sys.Initialize();
    auto status = sys.Console_GetStatus();
    EXPECT_TRUE(status.find("Initialized:") != std::string::npos);
    EXPECT_TRUE(status.find("YES") != std::string::npos);
    EXPECT_TRUE(status.find("Parallel Perception:") != std::string::npos);
    EXPECT_TRUE(status.find("Budget Limiter:") != std::string::npos);
    EXPECT_TRUE(status.find("NavMesh Obstacles:") != std::string::npos);
    sys.Shutdown();
}

TEST(AIIntegrated_ReinitializeAfterShutdown)
{
    ResetSingleton();
    auto& sys = AIIntegratedSystem::GetInstance();
    sys.Initialize();
    EXPECT_TRUE(sys.IsInitialized());
    sys.Shutdown();
    EXPECT_FALSE(sys.IsInitialized());
    sys.Initialize();
    EXPECT_TRUE(sys.IsInitialized());
    EXPECT_TRUE(sys.GetParallelPerception() != nullptr);
    sys.Shutdown();
}
