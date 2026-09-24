/**
 * @file TestMOD340PlatformerCompletionReal.cpp
 * @brief MOD-340: platform collision, kill plane, and automatic checkpoint restart in SparkGamePlatformer.
 *
 * Every test drives the real module classes in the order SparkGamePlatformerModule::OnFixedUpdate
 * uses (PlatformerLevelSystem::StepPlatforms, then PlatformerPlayerController::FixedUpdate) against
 * level 0 ("Green Hills"), plus a sweep of every level's spawn and checkpoints. No test presses the respawn key: restarts must come from the kill plane
 * and the game-over timer alone.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGamePlatformer/Source/Checkpoint/PlatformerCheckpointSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Level/PlatformerLevelSystem.h"
#include "../GameModules/SparkGamePlatformer/Source/Player/PlatformerPlayerController.h"

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

#endif // SPARK_TEST_HAS_IMGUI
