/**
 * @file TestTemplatesCompile.cpp
 * @brief Compile-time validation that every project template is real, working C++.
 *
 * Each template under `Templates/` is shipped as a concrete, compilable
 * `Spark::IModule` implementation — no `{{PROJECT_NAME}}`-style placeholders.
 * When a user scaffolds a new project via `ProjectManager::CreateProjectFromTemplate`,
 * the editor copies the template directory and rewrites every textual occurrence
 * of the template's name (e.g. `FPSStarter` → `MyGame`) in one pass.
 *
 * This file simply includes each template header so that the test binary fails
 * to build if a template's code ever rots. Instantiating each module class
 * and exercising its lifecycle makes sure the methods are real, not stubs.
 */

#include "TestFramework.h"

// Bring in every shipped template header.
#include "../Templates/EmptyProject/Source/GameModule.h"
#include "../Templates/Blank3D/Source/GameModule.h"
#include "../Templates/FPSStarter/Source/GameModule.h"
#include "../Templates/MMOStarter/Source/GameModule.h"
#include "../Templates/MultiplayerArena/Source/GameModule.h"
#include "../Templates/PlatformerKit/Source/GameModule.h"
#include "../Templates/RPGStarter/Source/GameModule.h"
#include "../Templates/ThirdPersonStarter/Source/GameModule.h"
#include "../Templates/TopDownStarter/Source/GameModule.h"

TEST(Templates_EmptyProject_ConstructsAndReportsInfo)
{
    EmptyProjectModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "EmptyProject");
    EXPECT_TRUE(info.version != nullptr && std::string(info.version) == "0.1.0");
    EXPECT_EQ(info.sdkVersion, static_cast<uint32_t>(SPARK_SDK_VERSION));
    EXPECT_EQ(info.loadOrder, 1000);

    EXPECT_TRUE(mod.OnLoad(nullptr));
    mod.OnUpdate(0.016f);
    EXPECT_EQ(mod.GetUpdateCount(), static_cast<uint64_t>(1));
    mod.OnPause();
    mod.OnUpdate(1.0f);
    EXPECT_EQ(mod.GetUpdateCount(), static_cast<uint64_t>(1));
    mod.OnResume();
    mod.OnUnload();
}

TEST(Templates_Blank3D_CameraControlsAndReset)
{
    Blank3DModule mod;
    EXPECT_TRUE(mod.OnLoad(nullptr));
    mod.Move(1.0f, -1.0f, 0.5f, 1.0f);
    mod.Look(20.0f, 100.0f);
    EXPECT_NEAR(mod.GetCameraState().z, -1.0f, 0.001f);
    EXPECT_NEAR(mod.GetCameraState().pitchDegrees, 89.0f, 0.001f);
    mod.ResetCamera();
    EXPECT_NEAR(mod.GetCameraState().z, -6.0f, 0.001f);
    mod.OnUnload();
}

TEST(Templates_FPSStarter_ConstructsAndRuns)
{
    FPSStarterModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "FPSStarter");

    EXPECT_TRUE(mod.OnLoad(nullptr));
    EXPECT_TRUE(mod.TryFire());
    EXPECT_EQ(mod.GetWeaponState().magazine, static_cast<uint32_t>(7));
    EXPECT_TRUE(mod.BeginReload());
    mod.OnUpdate(1.0f);
    EXPECT_EQ(mod.GetWeaponState().magazine, static_cast<uint32_t>(8));
    for (int shot = 0; shot < 3; ++shot)
    {
        EXPECT_TRUE(mod.TryFire());
        mod.OnUpdate(0.2f);
    }
    EXPECT_TRUE(mod.HasWonRound());
    mod.OnUnload();
}

TEST(Templates_ThirdPersonStarter_CompletesPickupAndGoalLoop)
{
    ThirdPersonStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(nullptr));
    EXPECT_TRUE(mod.Jump());
    EXPECT_FALSE(mod.Jump());
    mod.OnUpdate(1.0f);
    mod.Move(0.6f, 0.4f, 1.0f);
    EXPECT_TRUE(mod.TryCollectPickup());
    mod.Move(0.8f, 0.6f, 1.0f);
    EXPECT_TRUE(mod.TryReachGoal());
    EXPECT_TRUE(mod.GetState().goalReached);
    mod.OnUnload();
}

TEST(Templates_TopDownStarter_CompletesCombatLoop)
{
    TopDownStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(nullptr));
    mod.Move(1.0f, 1.0f, 1.0f);
    for (int attack = 0; attack < 4; ++attack)
        EXPECT_TRUE(mod.AttackEnemy());
    EXPECT_TRUE(mod.GetState().enemyDefeated);
    EXPECT_TRUE(mod.GetState().won);
    mod.OnUnload();
}

TEST(Templates_MMOStarter_CompletesBoundedLocalSession)
{
    MMOStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(nullptr));
    EXPECT_TRUE(mod.StartLocalSession());
    EXPECT_TRUE(mod.CreateCharacter("Astra"));
    EXPECT_TRUE(mod.SelectFaction(MMOStarterFaction::Azure));
    EXPECT_TRUE(mod.SubmitChat("Ready"));
    EXPECT_TRUE(mod.AdvanceCapture(5.0f));
    EXPECT_TRUE(mod.GetState().objectiveCaptured);
    EXPECT_EQ(mod.GetChatLog().size(), static_cast<size_t>(1));
    mod.OnUnload();
}

TEST(Templates_MultiplayerArena_ConstructsAndRuns)
{
    MultiplayerArenaModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "MultiplayerArena");

    EXPECT_TRUE(mod.OnLoad(nullptr));
    EXPECT_EQ(static_cast<uint8_t>(ArenaTeam::Cyan), static_cast<uint8_t>(1));
    EXPECT_EQ(static_cast<uint8_t>(ArenaTeam::Magenta), static_cast<uint8_t>(2));
    EXPECT_FALSE(mod.AddPlayer(10, "Unassigned", static_cast<uint8_t>(ArenaTeam::Unassigned)));
    EXPECT_FALSE(mod.AddPlayer(11, "Unknown", 3));
    EXPECT_TRUE(mod.AddPlayer(1, "Cyan", static_cast<uint8_t>(ArenaTeam::Cyan)));
    EXPECT_TRUE(mod.AddPlayer(2, "Magenta", static_cast<uint8_t>(ArenaTeam::Magenta)));
    EXPECT_TRUE(mod.SetReady(1, true));
    EXPECT_TRUE(mod.SetReady(2, true));
    mod.OnUpdate(0.016f);
    EXPECT_EQ(static_cast<int>(mod.GetMatchState().phase), static_cast<int>(MatchPhase::Countdown));
    mod.OnUpdate(5.0f);
    EXPECT_EQ(static_cast<int>(mod.GetMatchState().phase), static_cast<int>(MatchPhase::InProgress));
    EXPECT_TRUE(mod.RecordElimination(1, 2));
    EXPECT_EQ(mod.GetMatchState().teamCyanScore, static_cast<uint32_t>(1));
    EXPECT_EQ(mod.GetMatchState().teamMagentaScore, static_cast<uint32_t>(0));
    mod.OnUpdate(3.0f);
    EXPECT_TRUE(mod.GetPlayers()[1].isAlive);
    EXPECT_TRUE(mod.RecordElimination(2, 1));
    EXPECT_EQ(mod.GetMatchState().teamMagentaScore, static_cast<uint32_t>(1));
    mod.OnUnload();
}

TEST(Templates_PlatformerKit_ConstructsAndRuns)
{
    PlatformerKitModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "PlatformerKit");

    EXPECT_TRUE(mod.OnLoad(nullptr));
    EXPECT_TRUE(mod.Jump());
    EXPECT_TRUE(mod.Jump());
    EXPECT_FALSE(mod.Jump());
    EXPECT_TRUE(mod.CollectCoin(0));
    EXPECT_TRUE(mod.CollectCoin(1));
    EXPECT_TRUE(mod.CollectCoin(2));
    mod.ActivateCheckpoint(12.0f, 4.0f);
    mod.HitHazard();
    EXPECT_NEAR(mod.GetState().x, 12.0f, 0.001f);
    EXPECT_EQ(mod.GetState().lives, static_cast<uint32_t>(2));
    EXPECT_TRUE(mod.ReachFinish());
    mod.OnUnload();
}

TEST(Templates_RPGStarter_ConstructsAndRuns)
{
    RPGStarterModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "RPGStarter");

    EXPECT_TRUE(mod.OnLoad(nullptr));
    mod.TalkToElder();
    mod.CloseDialogue();
    EXPECT_TRUE(mod.PickUpRelic());
    EXPECT_TRUE(mod.AttackWarden());
    EXPECT_TRUE(mod.AttackWarden());
    EXPECT_TRUE(mod.AttackWarden());
    mod.TalkToElder();
    EXPECT_TRUE(mod.ClaimReward());
    mod.SaveToSlot();
    mod.NewGame();
    EXPECT_TRUE(mod.LoadFromSlot());
    EXPECT_TRUE(mod.HasItem("Lost Relic"));
    EXPECT_EQ(mod.GetState().gold, static_cast<uint32_t>(50));
    mod.OnUnload();
}
