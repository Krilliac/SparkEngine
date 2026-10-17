/**
 * @file TestMOD310FPSArenaAutopilotReal.cpp
 * @brief MOD-310: the arena autopilot's phases come from the production
 *        scoreboard, and it steers through the real camera and input paths.
 *
 * Every test drives the production FPSArenaAutopilot, GameMode, RespawnSystem,
 * EventBus, SparkEngineCamera and InputManager. The installed D3D11/WARP run
 * (Tests/PackageSmoke/RunInstalledFPSArenaLoop.cmake) plays the same autopilot
 * against the real arena; these tests pin the rules that run depends on:
 * a loop line can only count what GameMode and RespawnSystem recorded.
 */

#include "TestFramework.h"

#include "Game/FPSArenaAutopilot.h"
#include "Game/GameMechanics.h"
#include "Game/GameMode.h"

#include "Camera/SparkEngineCamera.h"
#include "Input/InputManager.h"
#include "Utils/EventBus.h"

#include <cmath>
#include <string>
#include <vector>

using SparkFPS::ArenaView;
using SparkFPS::FPSArenaAutopilot;
using Phase = SparkFPS::FPSArenaAutopilot::Phase;

namespace
{
    constexpr DirectX::XMFLOAT3 kNorthSpawn{0.0f, 2.0f, -20.0f};

    /// A Deathmatch match with the local player registered, as Game sets it up.
    struct ArenaMatch
    {
        Spark::EventBus bus;
        Spark::GameMode mode;
        Spark::RespawnSystem respawn;

        explicit ArenaMatch(bool withEventBus = true)
        {
            mode.Initialize(Spark::GameMode::GetPreset(Spark::GameModeType::Deathmatch));
            mode.AddPlayer(FPSArenaAutopilot::kPlayerName);
            mode.StartMatch();
            respawn.Initialize();
            if (withEventBus)
                respawn.SetEventBus(&bus);
            respawn.SetRespawnDelay(1.0f);
        }

        /// The route Game's player death callback takes (GameSetup.cpp).
        void KillLocalPlayer()
        {
            respawn.OnPlayerDeath("Enemy", "Unknown", false);
            mode.RecordKill("Enemy", FPSArenaAutopilot::kPlayerName);
        }

        /// The route Game's EntityKilledEvent handler takes (Game.cpp).
        void LocalPlayerKillsEnemy() { mode.RecordKill(FPSArenaAutopilot::kPlayerName, "Enemy"); }
    };

    float Yaw(const SparkEngineCamera& camera)
    {
        const DirectX::XMFLOAT3 forward = camera.GetForward();
        return std::atan2(forward.x, forward.z);
    }
} // namespace

TEST(FPSArenaAutopilot_PhasesFollowKillDeathAndPublishedRespawn)
{
    ArenaMatch match;
    FPSArenaAutopilot autopilot;
    autopilot.Enable(kNorthSpawn);

    EXPECT_TRUE(autopilot.UpdatePhase(match.mode, match.respawn) == Phase::Hunt);

    match.LocalPlayerKillsEnemy();
    EXPECT_TRUE(autopilot.UpdatePhase(match.mode, match.respawn) == Phase::Yield);

    // The death is recorded but its respawn is still counting down.
    match.KillLocalPlayer();
    EXPECT_TRUE(autopilot.UpdatePhase(match.mode, match.respawn) == Phase::Yield);
    EXPECT_EQ(FPSArenaAutopilot::ReadTally(match.mode, match.respawn).respawns, 0);

    // RespawnSystem publishes PlayerRespawnEvent once the delay elapses.
    match.respawn.Update(1.5f);
    ASSERT_FALSE(match.respawn.IsWaitingForRespawn());
    EXPECT_TRUE(autopilot.UpdatePhase(match.mode, match.respawn) == Phase::Hunt);

    match.LocalPlayerKillsEnemy();
    EXPECT_TRUE(autopilot.UpdatePhase(match.mode, match.respawn) == Phase::Complete);

    const std::string line = autopilot.FormatLoopLine(match.mode, match.respawn);
    EXPECT_STR_CONTAINS(line, "Loop: kills=2 deaths=1 respawns=1 score=200 moved=0.0 autopilot=complete");
}

TEST(FPSArenaAutopilot_CountsOnlyTheLocalPlayersScoreboardRow)
{
    ArenaMatch match;
    match.mode.AddPlayer("Bot");
    FPSArenaAutopilot autopilot;
    autopilot.Enable(kNorthSpawn);

    // Another player's kills, and kill-history records RespawnSystem keeps for
    // itself, are not the local player's kills.
    match.mode.RecordKill("Bot", "Enemy");
    match.mode.RecordKill("Bot", "Enemy");
    match.respawn.RecordKill("Player1", "Enemy", "Rifle", false, 10.0f);

    const SparkFPS::ArenaLoopTally tally = FPSArenaAutopilot::ReadTally(match.mode, match.respawn);
    EXPECT_EQ(tally.kills, 0);
    EXPECT_EQ(tally.score, 0);
    EXPECT_TRUE(autopilot.UpdatePhase(match.mode, match.respawn) == Phase::Hunt);
}

TEST(FPSArenaAutopilot_UnpublishedRespawnNeverCountsAsARespawn)
{
    // Without an event bus RespawnSystem cannot publish PlayerRespawnEvent, so
    // it keeps the death pending (the player stays dead). The loop must not
    // report that as a respawn, however long the countdown has run.
    ArenaMatch match(/*withEventBus*/ false);
    FPSArenaAutopilot autopilot;
    autopilot.Enable(kNorthSpawn);

    match.LocalPlayerKillsEnemy();
    match.KillLocalPlayer();
    match.respawn.Update(60.0f);
    match.LocalPlayerKillsEnemy();

    ASSERT_TRUE(match.respawn.IsWaitingForRespawn());
    const SparkFPS::ArenaLoopTally tally = FPSArenaAutopilot::ReadTally(match.mode, match.respawn);
    EXPECT_EQ(tally.kills, 2);
    EXPECT_EQ(tally.deaths, 1);
    EXPECT_EQ(tally.respawns, 0);
    EXPECT_TRUE(autopilot.UpdatePhase(match.mode, match.respawn) == Phase::Yield);
}

TEST(FPSArenaAutopilot_StepsTurnTheCameraAndHoldForwardThroughInput)
{
    ArenaMatch match;
    SparkEngineCamera camera;
    camera.Initialize(16.0f / 9.0f);
    InputManager input;
    FPSArenaAutopilot autopilot;

    // Off: nothing is driven.
    const std::vector<DirectX::XMFLOAT3> east{{10.0f, 1.0f, -20.0f}};
    const ArenaView view{kNorthSpawn, true, east};
    EXPECT_FALSE(autopilot.Step(match.mode, match.respawn, camera, input, view, 0.05f));
    EXPECT_FALSE(input.IsKeyDown('W'));
    EXPECT_NEAR(Yaw(camera), 0.0f, 0.001f);

    autopilot.Enable(kNorthSpawn);
    bool fired = false;
    for (int frame = 0; frame < 40 && !fired; ++frame)
        fired = autopilot.Step(match.mode, match.respawn, camera, input, view, 0.05f);

    // The target is 10 m due east: the camera turned to +90 degrees, W is held
    // because the target is beyond the hold distance, and it is in range to fire.
    EXPECT_TRUE(fired);
    EXPECT_NEAR(Yaw(camera), DirectX::XM_PIDIV2, FPSArenaAutopilot::kAimTolerance);
    EXPECT_TRUE(input.IsKeyDown('W'));

    // Close enough: stop walking into the target but keep firing.
    const std::vector<DirectX::XMFLOAT3> closeTargets{{5.0f, 1.0f, -20.0f}};
    EXPECT_TRUE(
        autopilot.Step(match.mode, match.respawn, camera, input, ArenaView{kNorthSpawn, true, closeTargets}, 0.05f));
    EXPECT_FALSE(input.IsKeyDown('W'));

    // Out of range: turn and walk, but do not waste ammunition.
    const std::vector<DirectX::XMFLOAT3> farTargets{{0.0f, 1.0f, 40.0f}};
    bool firedFar = false;
    for (int frame = 0; frame < 40; ++frame)
        firedFar =
            autopilot.Step(match.mode, match.respawn, camera, input, ArenaView{kNorthSpawn, true, farTargets}, 0.05f) ||
            firedFar;
    EXPECT_FALSE(firedFar);
    EXPECT_TRUE(input.IsKeyDown('W'));
    EXPECT_NEAR(Yaw(camera), 0.0f, FPSArenaAutopilot::kAimTolerance);

    // Yield (first kill, no death yet) releases W and holds fire.
    match.LocalPlayerKillsEnemy();
    EXPECT_FALSE(autopilot.Step(match.mode, match.respawn, camera, input, view, 0.05f));
    EXPECT_TRUE(autopilot.GetPhase() == Phase::Yield);
    EXPECT_FALSE(input.IsKeyDown('W'));

    // Disable releases a held key as well.
    match.KillLocalPlayer();
    match.respawn.Update(1.5f);
    autopilot.Step(match.mode, match.respawn, camera, input, ArenaView{kNorthSpawn, true, farTargets}, 0.05f);
    ASSERT_TRUE(input.IsKeyDown('W'));
    autopilot.Disable(input);
    EXPECT_FALSE(input.IsKeyDown('W'));
    EXPECT_FALSE(autopilot.IsEnabled());
}

TEST(FPSArenaAutopilot_MovementIsMeasuredPerLifeNotAcrossTheRespawnTeleport)
{
    ArenaMatch match;
    SparkEngineCamera camera;
    camera.Initialize(16.0f / 9.0f);
    InputManager input;
    FPSArenaAutopilot autopilot;
    autopilot.Enable(kNorthSpawn);

    const std::vector<DirectX::XMFLOAT3> none;
    const auto step = [&](DirectX::XMFLOAT3 position, bool alive)
    { autopilot.Step(match.mode, match.respawn, camera, input, ArenaView{position, alive, none}, 0.05f); };

    step({0.0f, 2.0f, -17.0f}, true);
    EXPECT_NEAR(autopilot.GetMaxDisplacement(), 3.0f, 0.001f);

    // A dead player's position does not count as movement.
    match.KillLocalPlayer();
    step({0.0f, 2.0f, 10.0f}, false);
    EXPECT_NEAR(autopilot.GetMaxDisplacement(), 3.0f, 0.001f);

    // The respawn moves the player 20+ m; that teleport is not movement either.
    match.respawn.Update(1.5f);
    step({20.0f, 2.0f, 0.0f}, true);
    EXPECT_NEAR(autopilot.GetMaxDisplacement(), 3.0f, 0.001f);

    step({20.0f, 2.0f, 4.0f}, true);
    EXPECT_NEAR(autopilot.GetMaxDisplacement(), 4.0f, 0.001f);
    EXPECT_STR_CONTAINS(autopilot.FormatLoopLine(match.mode, match.respawn),
                        "Loop: kills=0 deaths=1 respawns=1 score=0 moved=4.0 autopilot=hunt");
}
