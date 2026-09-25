/**
 * @file TestMOD340PlatformerCompletionReal.cpp
 * @brief MOD-340: platform collision, kill plane, and automatic checkpoint restart in SparkGamePlatformer.
 *
 * The collision tests drive the real module classes in the order PlatformerLevelFlow::StepFixed uses
 * (PlatformerLevelSystem::StepPlatforms, then PlatformerPlayerController::FixedUpdate) against level 0
 * ("Green Hills"), plus a sweep of every level's spawn and checkpoints. The scripted-run tests drive
 * PlatformerLevelFlow itself -- the same StepFrame/StepFixed the module's OnUpdate/OnFixedUpdate call --
 * with a route-following input script. No test presses the respawn key: restarts must come from the kill
 * plane and the game-over timer alone.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGamePlatformer/Source/Checkpoint/PlatformerCheckpointSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Collectible/PlatformerCollectibleSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Core/PlatformerLevelFlow.h"
#include "../GameModules/SparkGamePlatformer/Source/Hazard/PlatformerHazardSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Level/PlatformerLevelSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Player/PlatformerPlayerController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Platformer;

namespace
{
    constexpr float kFixedDt = 1.0f / 60.0f;

    // Level 0 platform indices (PlatformerLevelSystem::BuildGrasslandsLevel order)
    constexpr size_t kSteppingStoneOne = 1; // centre x 12, top 2
    constexpr size_t kMovingPlatform = 3;   // (28, 4) -> (40, 8)
    constexpr size_t kFirstFallingPlatform = 6;

    /// Level 0 loaded with a player bound to the real checkpoint and level systems.
    struct PlatformerRig
    {
        PlatformerLevelSystem level;
        PlatformerCheckpointSystem checkpoints;
        PlatformerPlayerController player;

        PlatformerRig()
        {
            level.Initialize(nullptr);
            level.LoadLevel(0);
            checkpoints.Initialize(nullptr);
            checkpoints.SetActiveLevel(0);
            player.Initialize(nullptr, &checkpoints, &level);
        }

        ~PlatformerRig()
        {
            player.Shutdown();
            checkpoints.Shutdown();
            level.Shutdown();
        }

        /// Place the player by respawning at a level spawn point (no checkpoint activated yet).
        void PlaceAt(float x, float y)
        {
            checkpoints.SetLevelSpawn(x, y, 0.0f);
            player.Respawn();
        }

        /// Simulate module frames: platforms step first, then the player, then per-frame timers.
        void Run(float seconds)
        {
            const int steps = static_cast<int>(std::lround(seconds / kFixedDt));
            for (int i = 0; i < steps; ++i)
            {
                level.StepPlatforms(kFixedDt);
                player.FixedUpdate(kFixedDt);
                player.Update(kFixedDt);
            }
        }
    };
} // namespace

TEST(PlatformerCompletion_PlayerLandsOnElevatedPlatform)
{
    PlatformerRig rig;
    const auto& stone = rig.level.GetActiveColliders().at(kSteppingStoneOne);
    EXPECT_NEAR(stone.maxY, 2.0f, 1e-4f);

    rig.PlaceAt(12.0f, 5.0f);
    rig.Run(1.5f);

    // The player rests on the stepping stone's top surface, not on an implicit y = 0 plane.
    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, 2.0f, 1e-4f);
    EXPECT_NEAR(rig.player.GetPlayerPosition().x, 12.0f, 1e-4f);
    EXPECT_EQ(rig.player.GetStateString(), std::string("Idle"));
    EXPECT_EQ(rig.player.GetLives(), 3);
}

TEST(PlatformerCompletion_FallingIntoGapRespawnsAtLastCheckpoint)
{
    PlatformerRig rig;

    // Drop into the gap between the starting platform (x <= 5) and the first stepping stone (x >= 10.5).
    rig.PlaceAt(8.0f, 3.0f);
    rig.checkpoints.CheckActivation(25.0f, 5.0f, 0.0f);
    ASSERT_EQ(rig.checkpoints.GetActivatedCount(), 1u);

    bool sawGapFloor = false;
    for (int i = 0; i < 60 && !sawGapFloor; ++i)
    {
        rig.Run(kFixedDt);
        sawGapFloor = rig.player.IsGrounded() && rig.player.GetPlayerPosition().x < 10.0f;
    }
    EXPECT_FALSE(sawGapFloor);

    rig.Run(4.0f);

    // The kill plane cost one life and restarted the player at checkpoint (25, 5) without the R key;
    // they then landed on the second stepping stone (top 4) beneath it.
    EXPECT_EQ(rig.player.GetLives(), 2);
    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().x, 25.0f, 1e-4f);
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, 4.0f, 1e-4f);
    EXPECT_TRUE(rig.player.GetStateString() != std::string("Dead"));
}

TEST(PlatformerCompletion_DeathAtZeroLivesRestartsAtCheckpoint)
{
    PlatformerRig rig;
    rig.PlaceAt(0.0f, 0.0f);
    rig.checkpoints.CheckActivation(55.0f, 13.0f, 0.0f);
    ASSERT_EQ(rig.checkpoints.GetActivatedCount(), 1u);

    // Three hazard hits, waiting out the respawn and hit invincibility frames before each one.
    rig.Run(2.0f);
    EXPECT_TRUE(rig.player.TakeDamage(1));
    rig.Run(2.0f);
    EXPECT_TRUE(rig.player.TakeDamage(1));
    rig.Run(2.0f);
    EXPECT_TRUE(rig.player.TakeDamage(1));
    EXPECT_EQ(rig.player.GetLives(), 0);
    EXPECT_EQ(rig.player.GetStateString(), std::string("Dead"));

    // Still dead during the restart delay...
    rig.Run(0.5f);
    EXPECT_EQ(rig.player.GetStateString(), std::string("Dead"));

    // ...then restarted automatically at checkpoint (55, 13) with fresh lives, landing on the high platform.
    rig.Run(2.0f);
    EXPECT_TRUE(rig.player.GetStateString() != std::string("Dead"));
    EXPECT_EQ(rig.player.GetLives(), 3);
    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().x, 55.0f, 1e-4f);
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, 12.0f, 1e-4f);
}

TEST(PlatformerCompletion_FallOnLastLifeRestartsAtCheckpoint)
{
    PlatformerRig rig;
    rig.PlaceAt(0.0f, 0.0f);
    rig.checkpoints.CheckActivation(25.0f, 5.0f, 0.0f);
    rig.Run(2.0f); // Outlast the respawn invincibility frames
    EXPECT_TRUE(rig.player.TakeDamage(1));
    rig.Run(2.0f);
    EXPECT_TRUE(rig.player.TakeDamage(1));
    rig.Run(2.0f);
    ASSERT_EQ(rig.player.GetLives(), 1);

    // Walk off the right edge of the starting platform and fall into the pit on the last life.
    rig.player.SetMovementInput(1.0f);
    rig.Run(0.8f);
    rig.player.SetMovementInput(0.0f);
    bool died = false;
    for (int i = 0; i < 240 && !died; ++i)
    {
        rig.Run(kFixedDt);
        died = rig.player.GetStateString() == "Dead";
    }
    ASSERT_TRUE(died);
    EXPECT_EQ(rig.player.GetLives(), 0);

    rig.Run(3.0f);
    EXPECT_EQ(rig.player.GetLives(), 3);
    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().x, 25.0f, 1e-4f);
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, 4.0f, 1e-4f);
}

TEST(PlatformerCompletion_MovingPlatformCarriesPlayer)
{
    PlatformerRig rig;
    const auto centreX = [&rig]
    {
        const auto& platform = rig.level.GetActiveColliders().at(kMovingPlatform);
        return (platform.minX + platform.maxX) * 0.5f;
    };

    rig.PlaceAt(28.0f, 4.5f);
    rig.Run(0.5f);
    ASSERT_TRUE(rig.player.IsGrounded());
    const float offsetOnPlatform = rig.player.GetPlayerPosition().x - centreX();

    rig.Run(2.5f);

    // The platform travelled towards (40, 8) and the idle player rode it at the same footing.
    const auto& platform = rig.level.GetActiveColliders().at(kMovingPlatform);
    EXPECT_GT(centreX(), 32.0f);
    EXPECT_GT(platform.maxY, 5.0f);
    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().x - centreX(), offsetOnPlatform, 1e-3f);
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, platform.maxY, 1e-3f);
}

TEST(PlatformerCompletion_FallingPlatformCollapsesUnderPlayer)
{
    PlatformerRig rig;
    rig.PlaceAt(65.0f, 13.0f);
    rig.Run(0.3f);
    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, 12.0f, 1e-4f);

    // After the 0.5 s fall delay the platform drops away with the player on it.
    rig.Run(0.7f);
    const auto& platform = rig.level.GetActiveColliders().at(kFirstFallingPlatform);
    EXPECT_LT(platform.maxY, 12.0f);
    EXPECT_LT(rig.player.GetPlayerPosition().y, 12.0f);
    EXPECT_EQ(rig.player.GetLives(), 3);
}

TEST(PlatformerCompletion_EveryCheckpointAndSpawnHoldsAnIdlePlayer)
{
    // Automatic restart is only safe if every restart point has ground beneath it: a checkpoint over
    // empty space drops the respawned player through the kill plane again, forever. Restart an idle
    // player at every level spawn and every checkpoint of every level and require them to survive.
    PlatformerLevelSystem level;
    level.Initialize(nullptr);
    ASSERT_TRUE(level.LoadLevel(0));
    level.CompleteLevel(1.0f, 0); // Three stars unlock every level

    PlatformerCheckpointSystem catalogue;
    catalogue.Initialize(nullptr);

    int restartPointsChecked = 0;
    for (uint32_t levelIndex = 0; levelIndex < level.GetLevelCount(); ++levelIndex)
    {
        ASSERT_TRUE(level.LoadLevel(levelIndex));
        const SpawnPoint spawn = level.GetCurrentSpawnPoint();

        // nullptr stands for the level spawn (no checkpoint activated).
        std::vector<const CheckpointData*> restartPoints{nullptr};
        for (const CheckpointData& cp : catalogue.GetCheckpoints())
        {
            if (cp.levelIndex == levelIndex)
                restartPoints.push_back(&cp);
        }

        for (const CheckpointData* cp : restartPoints)
        {
            ASSERT_TRUE(level.LoadLevel(levelIndex)); // Fresh platform state for each restart point
            PlatformerCheckpointSystem checkpoints;
            checkpoints.Initialize(nullptr);
            checkpoints.ResetLevel(levelIndex);
            checkpoints.SetLevelSpawn(spawn.x, spawn.y, spawn.z);
            if (cp)
                checkpoints.CheckActivation(cp->posX, cp->posY, cp->posZ);
            PlatformerPlayerController player;
            player.Initialize(nullptr, &checkpoints, &level);
            player.Respawn();

            const float expectedX = cp ? cp->posX : spawn.x;
            const float expectedZ = cp ? cp->posZ : spawn.z;
            EXPECT_NEAR(player.GetPlayerPosition().x, expectedX, 1e-4f);

            for (int i = 0; i < 180; ++i)
            {
                level.StepPlatforms(kFixedDt);
                player.FixedUpdate(kFixedDt);
                player.Update(kFixedDt);
            }

            const std::string where = "level " + std::to_string(levelIndex) + (cp ? " checkpoint " : " spawn ") +
                                      std::to_string(cp ? cp->id : 0u);
            if (player.GetLives() != 3 || player.GetStateString() == std::string("Dead"))
                std::printf("  restart point without ground: %s\n", where.c_str());
            EXPECT_EQ(player.GetLives(), 3);
            EXPECT_TRUE(player.GetStateString() != std::string("Dead"));
            EXPECT_GT(player.GetPlayerPosition().y, level.GetKillPlaneY());
            EXPECT_NEAR(player.GetPlayerPosition().z, expectedZ, 1e-3f);
            player.Shutdown();
            checkpoints.Shutdown();
            ++restartPointsChecked;
        }
    }

    // Three level spawns plus every placed checkpoint.
    EXPECT_EQ(static_cast<size_t>(restartPointsChecked), level.GetLevelCount() + catalogue.GetCheckpointCount());
    catalogue.Shutdown();
    level.Shutdown();
}

TEST(PlatformerCompletion_BouncyPadLaunchesWithoutHoldingJump)
{
    // The variable-jump cut shortens only ascents started with the jump button. A bouncy-pad launch with the
    // button up must keep its authored height (bounceForce 26 peaks ~11 m up), or the high platform (top 12)
    // after level 0's pad is unreachable and the level cannot be completed.
    PlatformerRig rig;
    constexpr size_t kBouncyPad = 4;
    const auto& pad = rig.level.GetActiveColliders().at(kBouncyPad);
    rig.PlaceAt((pad.minX + pad.maxX) * 0.5f, pad.maxY + 1.0f);

    float peak = rig.player.GetPlayerPosition().y;
    for (int i = 0; i < 90; ++i)
    {
        rig.Run(kFixedDt);
        peak = std::max(peak, rig.player.GetPlayerPosition().y);
    }
    EXPECT_GT(peak, 12.0f);
    EXPECT_EQ(rig.player.GetLives(), 3);
}

TEST(PlatformerCompletion_PlayerInsidePlatformIsPushedOntoItsTop)
{
    // A platform that moves, rotates, or reappears into the player leaves them inside its box, where the
    // face sweeps no longer apply. The push-out step must lift a player whose feet are just below a top
    // back onto it instead of letting them fall through into the pit.
    PlatformerRig rig;
    const auto& stone = rig.level.GetActiveColliders().at(kSteppingStoneOne);
    rig.PlaceAt(12.0f, stone.maxY - 0.2f);
    rig.Run(1.0f);

    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, 2.0f, 1e-4f);
    EXPECT_NEAR(rig.player.GetPlayerPosition().x, 12.0f, 1e-4f);
    EXPECT_EQ(rig.player.GetLives(), 3);
}

TEST(PlatformerCompletion_PlayerInsidePlatformSideIsPushedOutSideways)
{
    // Deep inside the left edge of the stepping stone (x 10.5..13.5, y 1.5..2), the shortest way out is
    // sideways: the player is pushed clear of the left face rather than teleported onto the top.
    PlatformerRig rig;
    rig.PlaceAt(10.7f, 1.0f);
    rig.Run(kFixedDt);

    EXPECT_LE(rig.player.GetPlayerPosition().x, 10.1f + 1e-4f);
    EXPECT_LT(rig.player.GetPlayerPosition().y, 1.0f + 1e-4f);
}

namespace
{
    /// Every gameplay system the module drives per frame, joined by the module's own PlatformerLevelFlow.
    struct PlatformerFlowRig
    {
        PlatformerLevelSystem level;
        PlatformerCheckpointSystem checkpoints;
        PlatformerPlayerController player;
        PlatformerCollectibleSystem collectibles;
        PlatformerHazardSystem hazards;
        PlatformerLevelFlow flow{level, player, collectibles, hazards, checkpoints};

        PlatformerFlowRig()
        {
            // Same initialization order as SparkGamePlatformerModule::OnLoad.
            level.Initialize(nullptr);
            checkpoints.Initialize(nullptr);
            player.Initialize(nullptr, &checkpoints, &level);
            collectibles.Initialize(nullptr);
            hazards.Initialize(nullptr);
        }

        ~PlatformerFlowRig()
        {
            hazards.Shutdown();
            collectibles.Shutdown();
            player.Shutdown();
            checkpoints.Shutdown();
            level.Shutdown();
        }
    };

    // Level 0 colliders are authored in route order: start, two stepping stones, moving platform, bouncy
    // pad, high platform, two falling platforms, two descent ledges, goal platform.
    constexpr size_t kGoalPlatform = 10;
    constexpr float kHalfWidth = 0.4f; // Player AABB half width (PlatformerPlayerCollision.cpp)
    constexpr float kGravity = 30.0f;  // PlatformerPlayerController gravity magnitude
    constexpr float kRunSpeed = 12.0f; // Move speed 8 x run multiplier 1.5

    /**
     * Scripted player: reads only what a human sees (player position, grounded state, platform boxes)
     * and writes only the controller's input API (SetMovementInput / SetJumpInput). It runs right along
     * the level 0 route, jumps at platform edges, steers in the air towards the next platform without
     * entering it from below, and waits for / rides the moving platform.
     */
    class RouteRunner
    {
      public:
        /// While finite, stand still at this x on the platform that contains it instead of advancing.
        float loiterX = std::nanf("");

        void Drive(PlatformerFlowRig& rig)
        {
            const PlayerPosition position = rig.player.GetPlayerPosition();
            const auto& colliders = rig.level.GetActiveColliders();

            // A checkpoint restart teleports the player: resume the route at the platform beneath them.
            if (std::abs(position.x - m_lastX) > 3.0f || std::abs(position.y - m_lastY) > 3.0f)
                m_routeIndex = RouteIndexBelow(colliders, position);
            m_lastX = position.x;
            m_lastY = position.y;

            // Route progress and footing are judged from position, not IsGrounded(): a bouncy pad never leaves
            // the player grounded, and an updraft briefly lifts a player riding the moving platform.
            const float verticalSpeed = rig.player.GetPlayerVelocity().y;
            const size_t touching = RouteIndexUnderFeet(colliders, position);
            if (touching != kNone)
                m_routeIndex = touching;
            const size_t standingOn = std::abs(verticalSpeed) < 1.0f ? touching : kNone;

            float move = 0.0f;
            bool jump = false;
            const size_t next = std::min(m_routeIndex + 1, kGoalPlatform);
            const PlatformCollider& target = colliders[next];
            const float targetCentre = (target.minX + target.maxX) * 0.5f;

            if (standingOn != kNone)
            {
                const PlatformCollider& ground = colliders[standingOn];
                const float groundCentre = (ground.minX + ground.maxX) * 0.5f;
                if (std::isfinite(loiterX) && position.x > ground.minX - kHalfWidth &&
                    position.x < ground.maxX + kHalfWidth && loiterX > ground.minX && loiterX < ground.maxX)
                {
                    move = Toward(position.x, loiterX);
                }
                else if (standingOn == kGoalPlatform)
                {
                    move = Toward(position.x, groundCentre);
                }
                else if (next == kMovingPlatform && target.minX > ground.maxX + 1.5f)
                {
                    // Wait near the edge until the moving platform comes back within jumping range.
                    move = Toward(position.x, ground.maxX - 1.0f);
                }
                else if (standingOn == kMovingPlatform && groundCentre < 38.5f)
                {
                    // Ride the moving platform to the far end of its track.
                    move = Toward(position.x, groundCentre);
                }
                else
                {
                    move = 1.0f;
                    jump = position.x >= ground.maxX - 0.6f;
                }
            }
            else
            {
                // Airborne: stay clear of the target's underside and side until above its top, and pick the
                // horizontal speed that arrives over the aim point when the fall reaches the target's top.
                float aimX = targetCentre;
                if (position.y < target.maxY + 0.05f)
                    aimX = std::min(aimX, target.minX - kHalfWidth - 0.3f);
                const float discriminant = verticalSpeed * verticalSpeed + 2.0f * kGravity * (position.y - target.maxY);
                const float timeToTop =
                    discriminant > 0.0f ? (verticalSpeed + std::sqrt(discriminant)) / kGravity : 0.25f;
                move = std::clamp((aimX - position.x) / std::max(timeToTop, 0.15f) / kRunSpeed, -1.0f, 1.0f);
                jump = verticalSpeed > 0.0f && m_jumpHeld;
            }

            // Release the button for one frame after a jump so the next press registers as a new jump.
            if (jump && m_jumpHeld && standingOn != kNone)
                jump = false;
            m_jumpHeld = jump;
            rig.player.SetMovementInput(move, true);
            rig.player.SetJumpInput(jump);
        }

      private:
        static constexpr size_t kNone = static_cast<size_t>(-1);

        static float Toward(float from, float to)
        {
            const float delta = to - from;
            if (std::abs(delta) < 0.1f)
                return 0.0f;
            return std::clamp(delta, -1.0f, 1.0f);
        }

        static size_t RouteIndexUnderFeet(const std::vector<PlatformCollider>& colliders, const PlayerPosition& p)
        {
            for (size_t i = 0; i <= kGoalPlatform; ++i)
            {
                const PlatformCollider& c = colliders[i];
                if (p.x + kHalfWidth > c.minX && p.x - kHalfWidth < c.maxX && std::abs(p.y - c.maxY) < 0.1f)
                    return i;
            }
            return kNone;
        }

        static size_t RouteIndexBelow(const std::vector<PlatformCollider>& colliders, const PlayerPosition& p)
        {
            for (size_t i = 0; i <= kGoalPlatform; ++i)
            {
                const PlatformCollider& c = colliders[i];
                if (p.x + kHalfWidth > c.minX && p.x - kHalfWidth < c.maxX && p.y >= c.maxY - 0.1f)
                    return i > 0 ? i - 1 : 0;
            }
            return 0;
        }

        size_t m_routeIndex = 0;
        float m_lastX = 0.0f;
        float m_lastY = 0.0f;
        bool m_jumpHeld = false;
    };

    /// Outcome of one scripted run of level 0 through PlatformerLevelFlow.
    struct ScriptedRun
    {
        bool completed = false;
        float completionTime = 0.0f; ///< PlatformerLevelSystem's level timer when the goal fired
        int hazardHits = 0;
        int hazardHitsWhileLoitering = 0;
        uint32_t pickups = 0;
        int gameOverRestarts = 0;
        bool restartedAtCheckpoint = false; ///< A game-over restart put the player on the last checkpoint
    };

    /**
     * Run level 0 at 60 Hz for at most @p maxSeconds of simulated time.
     * @param loiterAtSecondCheckpoint Stand in the sawblade's patrol at the second checkpoint (55, 13) once it
     *        activates, until hazards cause a game over, then carry on to the goal.
     */
    ScriptedRun RunLevel0(PlatformerFlowRig& rig, RouteRunner& runner, float maxSeconds, bool loiterAtSecondCheckpoint)
    {
        ScriptedRun run{};
        bool wasDead = false;
        const int frames = static_cast<int>(std::lround(maxSeconds / kFixedDt));
        for (int frame = 0; frame < frames && !run.completed; ++frame)
        {
            const bool loiter =
                loiterAtSecondCheckpoint && run.gameOverRestarts == 0 && rig.checkpoints.GetActivatedCount() >= 2;
            runner.loiterX = loiter ? rig.checkpoints.GetLastCheckpointPosition().x : std::nanf("");
            runner.Drive(rig);
            rig.flow.StepFixed(kFixedDt);
            const LevelFrameResult result = rig.flow.StepFrame(kFixedDt);
            run.hazardHits += result.hazardDamage > 0 ? 1 : 0;
            run.hazardHitsWhileLoitering += (loiter && result.hazardDamage > 0) ? 1 : 0;
            run.pickups += result.pickups;

            const bool dead = rig.player.GetStateString() == "Dead";
            if (wasDead && !dead)
            {
                ++run.gameOverRestarts;
                const PlayerPosition restart = rig.player.GetPlayerPosition();
                const PlayerPosition checkpoint = rig.checkpoints.GetLastCheckpointPosition();
                run.restartedAtCheckpoint = rig.checkpoints.GetActivatedCount() > 0 &&
                                            std::abs(restart.x - checkpoint.x) < 1e-4f &&
                                            std::abs(restart.y - checkpoint.y) < 1e-4f;
            }
            wasDead = dead;

            if (result.goalReached)
            {
                run.completed = true;
                run.completionTime = rig.level.GetLevelTimer();
            }
        }
        return run;
    }
} // namespace

TEST(PlatformerCompletion_ScriptedRunCompletesLevel0WithinThreeMinutes)
{
    PlatformerFlowRig rig;
    ASSERT_TRUE(rig.flow.StartLevel(0));
    ASSERT_TRUE(rig.level.IsLevelActive());

    RouteRunner runner;
    const ScriptedRun run = RunLevel0(rig, runner, 180.0f, false);
    std::printf("  level 0 scripted run: completed=%d time=%.2fs hazardHits=%d pickups=%u restarts=%d lives=%d\n",
                run.completed ? 1 : 0, run.completionTime, run.hazardHits, run.pickups, run.gameOverRestarts,
                rig.player.GetLives());

    ASSERT_TRUE(run.completed);
    EXPECT_LE(run.completionTime, 180.0f);
    EXPECT_FALSE(rig.level.IsLevelActive());

    // The flow drove collection and checkpoints on the way: every level 0 checkpoint is on the route.
    EXPECT_GT(run.pickups, 0u);
    EXPECT_GT(rig.collectibles.GetCoinsCollected(), 0);
    EXPECT_EQ(rig.checkpoints.GetActivatedCount(), 3u);

    // The goal fired at the goal platform, not from below it: the player settles on its top.
    for (int i = 0; i < 60; ++i)
    {
        runner.Drive(rig);
        rig.flow.StepFixed(kFixedDt);
        rig.flow.StepFrame(kFixedDt);
    }
    const PlatformCollider& goal = rig.level.GetActiveColliders().at(kGoalPlatform);
    EXPECT_TRUE(rig.player.IsGrounded());
    EXPECT_NEAR(rig.player.GetPlayerPosition().y, goal.maxY, 1e-3f);
    EXPECT_GE(rig.player.GetPlayerPosition().x, goal.minX);
    EXPECT_LE(rig.player.GetPlayerPosition().x, goal.maxX);

    // Completion is recorded as level progress and unlocks the next level.
    const std::string levels = rig.level.GetLevelListString();
    EXPECT_TRUE(levels.find("Green Hills (11 platforms) [DONE]") != std::string::npos);
}

TEST(PlatformerCompletion_HazardDeathMidRunRestartsAtCheckpointAndStillCompletes)
{
    PlatformerFlowRig rig;
    ASSERT_TRUE(rig.flow.StartLevel(0));

    // Once the second checkpoint (55, 13) is live the player stands in the sawblade's patrol; the flow's
    // hazard checks must take every life, the game-over timer must restart them at that checkpoint, and the
    // scripted run must still reach the goal inside the three-minute budget.
    RouteRunner runner;
    const ScriptedRun run = RunLevel0(rig, runner, 180.0f, true);
    std::printf("  level 0 run with game over: completed=%d time=%.2fs hazardHits=%d (loitering %d) restarts=%d "
                "atCheckpoint=%d\n",
                run.completed ? 1 : 0, run.completionTime, run.hazardHits, run.hazardHitsWhileLoitering,
                run.gameOverRestarts, run.restartedAtCheckpoint ? 1 : 0);

    EXPECT_GE(run.hazardHitsWhileLoitering, 1);
    EXPECT_GE(run.gameOverRestarts, 1);
    EXPECT_TRUE(run.restartedAtCheckpoint);
    ASSERT_TRUE(run.completed);
    EXPECT_LE(run.completionTime, 180.0f);
}

#endif // SPARK_TEST_HAS_IMGUI
