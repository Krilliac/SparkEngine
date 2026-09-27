/**
 * @file TestMOD360OpenWorldTraversalReal.cpp
 * @brief MOD-360: automated open-world traversal driven only through OWPlayerController input.
 *
 * The real OpenWorld gameplay systems are stepped at a fixed 1/60 s in the order
 * SparkGameOpenWorldModule::OnUpdate / OnFixedUpdate run them. Movement, sprint, and every
 * gather/event/settlement interaction go through the controller; the scripted route crosses
 * from Emerald Meadows (region 1) into Ironwood Forest (region 2). This runs in-process with
 * no renderer or packaged build.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameOpenWorld/Source/Events/OWDynamicEventSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Exploration/OWExplorationSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Gathering/OWGatheringSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Player/OWPlayerController.h"
#include "../GameModules/SparkGameOpenWorld/Source/Player/OWPlayerSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Settlement/OWSettlementSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Wildlife/OWWildlifeSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/World/OWWorldSetup.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>

using namespace OpenWorld;

namespace
{
    constexpr float kTraversalStep = 1.0f / 60.0f;

    /// Every OpenWorld gameplay system plus the controller, as SparkGameOpenWorldModule owns them.
    struct TraversalSession
    {
        OWWorldSetup world;
        OWPlayerSystem player;
        OWExplorationSystem exploration;
        OWWildlifeSystem wildlife;
        OWSettlementSystem settlements;
        OWGatheringSystem gathering;
        OWDynamicEventSystem events;
        OWPlayerController controller;

        float simulatedSeconds = 0.0f;
        std::set<uint32_t> regionsVisited;
        uint32_t watchedEventId = 0;
        std::optional<ActiveWorldEvent> watchedEventLastSeen;
        bool watchedEventCompleted = false;

        TraversalSession()
        {
            // Context-free, as the Gated_OW* tests construct them: no host input, so the
            // controller keeps the input the test injects.
            m_started = world.Initialize(nullptr) && player.Initialize(nullptr) && exploration.Initialize(nullptr) &&
                        wildlife.Initialize(nullptr) && settlements.Initialize(nullptr) &&
                        gathering.Initialize(nullptr) && events.Initialize(nullptr);
            controller.Initialize(nullptr, player, world, gathering, events, settlements);
        }

        ~TraversalSession()
        {
            events.Shutdown();
            gathering.Shutdown();
            settlements.Shutdown();
            wildlife.Shutdown();
            exploration.Shutdown();
            player.Shutdown();
            world.Shutdown();
        }

        TraversalSession(const TraversalSession&) = delete;
        TraversalSession& operator=(const TraversalSession&) = delete;

        bool Started() const { return m_started; }

        /// Controller and player fixed step only (the OnFixedUpdate pair).
        void FixedStep()
        {
            controller.FixedUpdate(kTraversalStep);
            player.FixedUpdate(kTraversalStep);
        }

        /// One frame in SparkGameOpenWorldModule::OnUpdate then OnFixedUpdate order.
        void StepFrame()
        {
            controller.Update(kTraversalStep);
            world.Update(kTraversalStep);
            player.Update(kTraversalStep);
            const PlayerWorldState& position = player.GetWorldState();
            const BiomeRegion* region = world.GetRegionAtPosition(position.posX, position.posY, position.posZ);
            player.SetCurrentRegion(region ? region->regionId : 0);
            const PlayerWorldState& state = player.GetWorldState();
            exploration.Update(kTraversalStep, state.posX, state.posY, state.posZ);
            wildlife.Update(kTraversalStep, state.posX, state.posZ, state.currentRegionId);
            settlements.Update(kTraversalStep);
            gathering.Update(kTraversalStep);
            events.Update(kTraversalStep, state.posX, state.posZ, state.currentRegionId);
            FixedStep();

            simulatedSeconds += kTraversalStep;
            regionsVisited.insert(player.GetWorldState().currentRegionId);
            ObserveWatchedEvent();
        }

        /// Turn toward (x, z) and walk there through controller input, sprinting while stamina
        /// lasts. Food and water come from the player's supplies (the ow_eat / ow_drink actions).
        bool WalkTo(float x, float z, float arriveWithin, float budgetSeconds)
        {
            while (simulatedSeconds < budgetSeconds)
            {
                const PlayerWorldState& state = player.GetWorldState();
                const float dx = x - state.posX;
                const float dz = z - state.posZ;
                if (std::sqrt(dx * dx + dz * dz) <= arriveWithin)
                {
                    controller.SetMoveInput(0.0f, 0.0f, false);
                    controller.SetTurnInput(0.0f);
                    return true;
                }

                const float desiredYaw = std::atan2(dx, dz) * 180.0f / 3.14159265358979f;
                const float yawError = std::fmod(desiredYaw - state.yaw + 540.0f, 360.0f) - 180.0f;
                controller.SetTurnInput(std::clamp(yawError * 6.0f, -360.0f, 360.0f));
                const bool facingTarget = std::abs(yawError) < 45.0f;
                controller.SetMoveInput(facingTarget ? 1.0f : 0.0f, 0.0f, player.GetSurvivalState().stamina > 30.0f);

                if (player.GetSurvivalState().thirst < 40.0f)
                    player.Drink(40.0f);
                if (player.GetSurvivalState().hunger < 40.0f)
                    player.Eat(30.0f);
                StepFrame();
            }
            return false;
        }

        /// Step with no input until the watched event has run its course.
        bool WaitForWatchedEvent(float budgetSeconds)
        {
            controller.SetMoveInput(0.0f, 0.0f, false);
            controller.SetTurnInput(0.0f);
            while (!watchedEventCompleted && simulatedSeconds < budgetSeconds)
                StepFrame();
            return watchedEventCompleted;
        }

        void Place(float x, float z, float yaw)
        {
            const BiomeRegion* region = world.GetRegionAtPosition(x, z);
            player.SetPosition(x, region ? region->elevationMin : 0.0f, z);
            player.SetCurrentRegion(region ? region->regionId : 0);
            player.SetFacing(yaw);
        }

        uint32_t NodeYield(uint32_t nodeId) const
        {
            for (const auto& node : gathering.CaptureSaveState().nodes)
            {
                if (node.nodeId == nodeId)
                    return node.currentYield;
            }
            return 0;
        }

      private:
        void ObserveWatchedEvent()
        {
            if (watchedEventId == 0 || watchedEventCompleted)
                return;
            for (const auto& event : events.CaptureSaveState().activeEvents)
            {
                if (event.eventId == watchedEventId)
                {
                    watchedEventLastSeen = event;
                    return;
                }
            }
            // CompleteEvent removes an event once its duration has run out.
            watchedEventCompleted = watchedEventLastSeen.has_value();
        }

        bool m_started = false;
    };

    // Authored data these tests route through (OWGatheringSystem / OWSettlementSystem definitions).
    constexpr uint32_t kOakTreeNode = 1;      // (200, 100), region 1
    constexpr uint32_t kIronwoodTreeNode = 6; // (3000, -200), region 2
    constexpr uint32_t kMeadowbrook = 1;      // centre (100, -100), region 1
    constexpr uint32_t kTimberhold = 2;       // centre (2800, 0), region 2
    constexpr uint32_t kHarvestFestival = 7;  // 240 s event template
} // namespace

TEST(OpenWorldTraversal_ControllerWalksAndSprintsAtFixedRates)
{
    TraversalSession session;
    ASSERT_TRUE(session.Started());
    session.Place(0.0f, 0.0f, 0.0f);

    // Walk north (+Z) for one second.
    session.controller.SetMoveInput(1.0f, 0.0f, false);
    for (int i = 0; i < 60; ++i)
        session.FixedStep();
    EXPECT_NEAR(session.player.GetWorldState().posZ, OWPlayerController::kWalkSpeed, 1e-3f);
    EXPECT_NEAR(session.player.GetWorldState().posX, 0.0f, 1e-3f);
    EXPECT_NEAR(session.player.GetWorldState().speed, OWPlayerController::kWalkSpeed, 1e-4f);
    EXPECT_FALSE(session.player.GetWorldState().isSprinting);
    EXPECT_NEAR(session.player.GetSurvivalState().stamina, 100.0f, 1e-4f);
    EXPECT_EQ(session.player.GetWorldState().currentRegionId, static_cast<uint32_t>(1));
    EXPECT_NEAR(session.player.GetWorldState().posY, 0.0f, 1e-4f); // Emerald Meadows ground plane

    // Sprint for one second: sprint speed, and the player system drains 15 stamina/s.
    session.controller.SetMoveInput(1.0f, 0.0f, true);
    for (int i = 0; i < 60; ++i)
        session.FixedStep();
    EXPECT_NEAR(session.player.GetWorldState().posZ, OWPlayerController::kWalkSpeed + OWPlayerController::kSprintSpeed,
                1e-3f);
    EXPECT_TRUE(session.player.GetWorldState().isSprinting);
    EXPECT_NEAR(session.player.GetSurvivalState().stamina, 85.0f, 1e-2f);

    // Turn in place to face east, then walk: movement follows the facing.
    session.controller.SetMoveInput(0.0f, 0.0f, false);
    session.controller.SetTurnInput(90.0f);
    for (int i = 0; i < 60; ++i)
        session.FixedStep();
    EXPECT_NEAR(session.player.GetWorldState().yaw, 90.0f, 1e-2f);
    EXPECT_NEAR(session.player.GetWorldState().speed, 0.0f, 1e-6f);
    session.controller.SetTurnInput(0.0f);
    session.controller.SetMoveInput(1.0f, 0.0f, false);
    for (int i = 0; i < 60; ++i)
        session.FixedStep();
    EXPECT_NEAR(session.player.GetWorldState().posX, OWPlayerController::kWalkSpeed, 1e-2f);

    // A dead player does not move.
    session.player.TakeDamage(1000.0f);
    const float deadX = session.player.GetWorldState().posX;
    for (int i = 0; i < 60; ++i)
        session.FixedStep();
    EXPECT_NEAR(session.player.GetWorldState().posX, deadX, 1e-6f);
    EXPECT_NEAR(session.player.GetWorldState().speed, 0.0f, 1e-6f);
}

TEST(OpenWorldTraversal_SprintStopsWhenStaminaExhausted)
{
    TraversalSession session;
    ASSERT_TRUE(session.Started());
    session.Place(0.0f, 0.0f, 0.0f);
    session.controller.SetMoveInput(1.0f, 0.0f, true);

    int steps = 0;
    while (session.player.GetSurvivalState().stamina > 0.0f && steps < 600)
    {
        session.FixedStep();
        ++steps;
    }
    ASSERT_TRUE(session.player.GetSurvivalState().stamina <= 0.0f);
    EXPECT_GE(steps, 399); // 100 stamina at 15/s is ~6.67 s of sprinting
    EXPECT_LE(steps, 402);

    // Sprint stays held, but an exhausted player walks until stamina recovers to the threshold.
    int walkedSteps = 0;
    while (session.player.GetSurvivalState().stamina < OWPlayerController::kSprintRecoverStamina && walkedSteps < 600)
    {
        const float beforeZ = session.player.GetWorldState().posZ;
        session.FixedStep();
        ++walkedSteps;
        EXPECT_FALSE(session.player.GetWorldState().isSprinting);
        EXPECT_NEAR(session.player.GetWorldState().posZ - beforeZ, OWPlayerController::kWalkSpeed * kTraversalStep,
                    1e-3f);
    }
    EXPECT_GE(walkedSteps, 180); // 25 stamina at 8/s is ~3.1 s
    EXPECT_LE(walkedSteps, 200);

    const float beforeZ = session.player.GetWorldState().posZ;
    session.FixedStep();
    EXPECT_TRUE(session.player.GetWorldState().isSprinting);
    EXPECT_NEAR(session.player.GetWorldState().posZ - beforeZ, OWPlayerController::kSprintSpeed * kTraversalStep,
                1e-3f);
}

TEST(OpenWorldTraversal_WorldEdgeRejectsMove)
{
    TraversalSession session;
    ASSERT_TRUE(session.Started());
    // Emerald Meadows ends at z = -2000 and no region covers the band south of it at x = 0.
    ASSERT_TRUE(session.world.GetRegionAtPosition(0.0f, -1999.95f) != nullptr);
    ASSERT_TRUE(session.world.GetRegionAtPosition(0.0f, -2000.05f) == nullptr);
    session.Place(0.0f, -1999.95f, 180.0f);

    session.controller.SetMoveInput(1.0f, 0.0f, true);
    for (int i = 0; i < 30; ++i)
        session.FixedStep();
    EXPECT_NEAR(session.player.GetWorldState().posZ, -1999.95f, 1e-3f);
    EXPECT_NEAR(session.player.GetWorldState().posX, 0.0f, 1e-3f);
    EXPECT_NEAR(session.player.GetWorldState().speed, 0.0f, 1e-6f);
    EXPECT_FALSE(session.player.GetWorldState().isSprinting);
    EXPECT_EQ(session.player.GetWorldState().currentRegionId, static_cast<uint32_t>(1));

    // Turning back inland moves again.
    session.player.SetFacing(0.0f);
    session.FixedStep();
    EXPECT_GT(session.player.GetWorldState().posZ, -1999.95f);
}

TEST(OpenWorldTraversal_InteractHarvestsNearestNodeOnly)
{
    TraversalSession session;
    ASSERT_TRUE(session.Started());

    // 3 m from the Oak Tree at (200, 100): inside the 4 m harvest range.
    session.Place(203.0f, 100.0f, 0.0f);
    const InteractResult harvest = session.controller.TryInteract();
    EXPECT_TRUE(harvest.kind == InteractResult::Kind::Harvest);
    EXPECT_EQ(harvest.id, kOakTreeNode);
    EXPECT_EQ(harvest.amount, static_cast<uint32_t>(1));
    EXPECT_EQ(session.gathering.GetInventory().Get(ResourceType::Wood), static_cast<uint32_t>(1));
    EXPECT_EQ(session.NodeYield(kOakTreeNode), static_cast<uint32_t>(4));

    // 10 m away nothing is in reach: no node, event, or settlement.
    session.Place(210.0f, 100.0f, 0.0f);
    const InteractResult nothing = session.controller.TryInteract();
    EXPECT_TRUE(nothing.kind == InteractResult::Kind::None);
    EXPECT_EQ(nothing.id, static_cast<uint32_t>(0));
    EXPECT_EQ(session.gathering.GetInventory().Get(ResourceType::Wood), static_cast<uint32_t>(1));
    EXPECT_EQ(session.NodeYield(kOakTreeNode), static_cast<uint32_t>(4));
}

TEST(OpenWorldTraversal_CrossesTwoRegionsAndCompletesGatherEventSettlement)
{
    TraversalSession session;
    ASSERT_TRUE(session.Started());
    constexpr float kBudgetSeconds = 900.0f;
    ASSERT_EQ(session.player.GetWorldState().currentRegionId, static_cast<uint32_t>(1));

    // Meadowbrook: walk into the village and interact to visit it.
    ASSERT_TRUE(session.WalkTo(100.0f, -100.0f, 5.0f, kBudgetSeconds));
    const InteractResult meadowbrook = session.controller.TryInteract();
    EXPECT_TRUE(meadowbrook.kind == InteractResult::Kind::VisitSettlement);
    EXPECT_EQ(meadowbrook.id, kMeadowbrook);

    // Oak Tree: harvest wood.
    ASSERT_TRUE(session.WalkTo(200.0f, 100.0f, 2.0f, kBudgetSeconds));
    const InteractResult oak = session.controller.TryInteract();
    EXPECT_TRUE(oak.kind == InteractResult::Kind::Harvest);
    EXPECT_EQ(oak.id, kOakTreeNode);

    // Head east across x = 2000 into Ironwood Forest.
    ASSERT_TRUE(session.WalkTo(2010.0f, -150.0f, 3.0f, kBudgetSeconds));
    EXPECT_EQ(session.player.GetWorldState().currentRegionId, static_cast<uint32_t>(2));

    // Timberhold starts its harvest festival as the player enters the forest.
    session.watchedEventId = session.events.TriggerEvent(kHarvestFestival, 2, 2800.0f, 0.0f);
    ASSERT_TRUE(session.watchedEventId != 0);

    // Ironwood Tree: harvest wood in the second region.
    ASSERT_TRUE(session.WalkTo(3000.0f, -200.0f, 2.0f, kBudgetSeconds));
    const InteractResult ironwood = session.controller.TryInteract();
    EXPECT_TRUE(ironwood.kind == InteractResult::Kind::Harvest);
    EXPECT_EQ(ironwood.id, kIronwoodTreeNode);

    // Timberhold: the running festival takes interaction priority over the settlement,
    // and once joined the next interaction visits the settlement.
    ASSERT_TRUE(session.WalkTo(2800.0f, 0.0f, 3.0f, kBudgetSeconds));
    const InteractResult festival = session.controller.TryInteract();
    EXPECT_TRUE(festival.kind == InteractResult::Kind::JoinEvent);
    EXPECT_EQ(festival.id, session.watchedEventId);
    const InteractResult timberhold = session.controller.TryInteract();
    EXPECT_TRUE(timberhold.kind == InteractResult::Kind::VisitSettlement);
    EXPECT_EQ(timberhold.id, kTimberhold);

    // The festival runs to completion with the player participating.
    ASSERT_TRUE(session.WaitForWatchedEvent(kBudgetSeconds));
    ASSERT_TRUE(session.watchedEventLastSeen.has_value());
    EXPECT_TRUE(session.watchedEventLastSeen->playerParticipating);
    EXPECT_TRUE(session.watchedEventLastSeen->state == EventState::Resolving);
    EXPECT_GE(session.events.CaptureSaveState().totalEventsCompleted, static_cast<uint32_t>(1));

    EXPECT_TRUE(session.regionsVisited.count(1) == 1);
    EXPECT_TRUE(session.regionsVisited.count(2) == 1);
    EXPECT_EQ(session.gathering.GetInventory().Get(ResourceType::Wood), static_cast<uint32_t>(2));
    EXPECT_EQ(session.NodeYield(kOakTreeNode), static_cast<uint32_t>(4));
    EXPECT_EQ(session.NodeYield(kIronwoodTreeNode), static_cast<uint32_t>(7));
    EXPECT_TRUE(session.settlements.IsSettlementVisited(kMeadowbrook));
    EXPECT_TRUE(session.settlements.IsSettlementVisited(kTimberhold));
    EXPECT_EQ(session.settlements.GetVisitedSettlementCount(), static_cast<size_t>(2));
    EXPECT_TRUE(session.player.IsAlive());
    EXPECT_LT(session.simulatedSeconds, kBudgetSeconds);
}

#endif // SPARK_TEST_HAS_IMGUI
