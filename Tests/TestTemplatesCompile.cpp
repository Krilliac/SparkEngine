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
#include "Core/EngineContext.h"

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

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace
{
    class ScopedCurrentPath
    {
      public:
        explicit ScopedCurrentPath(const std::filesystem::path& path) : m_previous(std::filesystem::current_path())
        {
            std::filesystem::current_path(path);
        }

        ~ScopedCurrentPath()
        {
            std::error_code ec;
            std::filesystem::current_path(m_previous, ec);
        }

        ScopedCurrentPath(const ScopedCurrentPath&) = delete;
        ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

      private:
        std::filesystem::path m_previous;
    };

    std::filesystem::path FPSStarterProjectRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path() / "Templates" / "FPSStarter";
    }
} // namespace

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

TEST(Templates_FPSStarter_MissesConsumeAmmoWithoutDamagingTarget)
{
    FPSStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(nullptr));
    EXPECT_TRUE(mod.TryFire(false));
    EXPECT_EQ(mod.GetWeaponState().magazine, static_cast<uint32_t>(7));
    EXPECT_NEAR(mod.GetTargetState().health, 100.0f, 0.001f);
    mod.OnUpdate(0.2f);
    EXPECT_TRUE(mod.TryFire(true));
    EXPECT_NEAR(mod.GetTargetState().health, 75.0f, 0.001f);
    EXPECT_FALSE(mod.SupportsHotReload());
    mod.OnUnload();
}

TEST(Templates_FPSStarter_ControlMathIsNormalizedAndFrameRateIndependent)
{
    const DirectX::XMFLOAT3 diagonal = FPSStarterModule::ComputePlanarMovement(1.0f, 1.0f, 0.0f, 5.0f, 1.0f);
    EXPECT_NEAR(std::sqrt(diagonal.x * diagonal.x + diagonal.z * diagonal.z), 5.0f, 0.001f);

    const DirectX::XMFLOAT3 whole = FPSStarterModule::ComputePlanarMovement(1.0f, 0.0f, 37.0f, 5.0f, 1.0f);
    const DirectX::XMFLOAT3 half = FPSStarterModule::ComputePlanarMovement(1.0f, 0.0f, 37.0f, 5.0f, 0.5f);
    EXPECT_NEAR(whole.x, half.x * 2.0f, 0.001f);
    EXPECT_NEAR(whole.z, half.z * 2.0f, 0.001f);
    EXPECT_NEAR(FPSStarterModule::ClampPitch(120.0f), 89.0f, 0.001f);
    EXPECT_NEAR(FPSStarterModule::ClampPitch(-120.0f), -89.0f, 0.001f);
}

TEST(Templates_FPSStarter_CrosshairRayUsesTargetBounds)
{
    const DirectX::XMFLOAT3 origin{0.0f, 1.7f, 0.0f};
    const DirectX::XMFLOAT3 targetMin{-0.5f, 0.0f, 5.5f};
    const DirectX::XMFLOAT3 targetMax{0.5f, 2.0f, 6.5f};
    EXPECT_TRUE(FPSStarterModule::RayIntersectsAabb(origin, {0.0f, 0.0f, 1.0f}, targetMin, targetMax));
    EXPECT_FALSE(FPSStarterModule::RayIntersectsAabb(origin, {1.0f, 0.0f, 0.0f}, targetMin, targetMax));
}

TEST(Templates_FPSStarter_ArenaContainsRuntimeContract)
{
    const std::filesystem::path arena = FPSStarterProjectRoot() / "Scenes" / "Arena.sparkscene";
    World world;
    EXPECT_TRUE(Spark::LoadWorld(world, arena.string()));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(6));

    bool foundCamera = false;
    bool foundPlayer = false;
    bool foundTarget = false;
    for (EntityID entity : world.GetEntitiesWith<NameComponent>())
    {
        const NameComponent* named = world.GetComponent<NameComponent>(entity);
        if (!named)
            continue;
        if (named->name == "Main Camera")
            foundCamera = world.HasComponent<Transform>(entity) && world.HasComponent<Camera>(entity) &&
                          world.GetComponent<Camera>(entity)->isMainCamera;
        else if (named->name == "Player")
            foundPlayer =
                world.HasComponent<Transform>(entity) && world.HasComponent<CharacterControllerComponent>(entity);
        else if (named->name == "Damageable Target")
            foundTarget = world.HasComponent<Transform>(entity) && world.HasComponent<MeshRenderer>(entity);
    }
    EXPECT_TRUE(foundCamera);
    EXPECT_TRUE(foundPlayer);
    EXPECT_TRUE(foundTarget);
}

TEST(Templates_FPSStarter_HeadlessContextLoadsAndCleansRuntimeScene)
{
    ScopedCurrentPath projectRoot(FPSStarterProjectRoot());
    World world;
    EngineContext context;
    context.SetWorld(&world);

    FPSStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(&context));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(6));
    EXPECT_TRUE(mod.TryFire());
    mod.OnUpdate(0.2f);
    EXPECT_NEAR(mod.GetTargetState().health, 75.0f, 0.001f);

    mod.OnUnload();
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(0));
}

TEST(Templates_FPSStarter_MouseCaptureTransitionsSuppressFireWithoutPlatformInput)
{
    const FPSStarterCaptureTransition win32AutoCapture =
        FPSStarterModule::ComputeCaptureTransition(true, false, true, false, false, false, false);
    EXPECT_FALSE(win32AutoCapture.captureChanged);
    EXPECT_TRUE(win32AutoCapture.captureMouse);
    EXPECT_TRUE(win32AutoCapture.suppressFire);

    const FPSStarterCaptureTransition released =
        FPSStarterModule::ComputeCaptureTransition(true, true, false, true, false, false, true);
    EXPECT_FALSE(released.captureChanged);
    EXPECT_FALSE(released.suppressFire);

    const FPSStarterCaptureTransition escape =
        FPSStarterModule::ComputeCaptureTransition(true, true, false, false, true, false, false);
    EXPECT_TRUE(escape.captureChanged);
    EXPECT_FALSE(escape.captureMouse);

    const FPSStarterCaptureTransition recapture =
        FPSStarterModule::ComputeCaptureTransition(false, false, true, false, false, false, false);
    EXPECT_TRUE(recapture.captureChanged);
    EXPECT_TRUE(recapture.captureMouse);
    EXPECT_TRUE(recapture.suppressFire);
}

TEST(Templates_FPSStarter_RuntimeOwnsOnlyLoadedNamedEntities)
{
    ScopedCurrentPath projectRoot(FPSStarterProjectRoot());
    World world;
    auto& registry = world.GetRegistry();

    const EntityID hostCamera = registry.create(static_cast<EntityID>(100));
    registry.emplace<NameComponent>(hostCamera, NameComponent{"Main Camera"});
    world.AddComponent<Transform>(hostCamera).position = {91.0f, 92.0f, 93.0f};
    world.AddComponent<Camera>(hostCamera);

    const EntityID hostPlayer = registry.create(static_cast<EntityID>(101));
    registry.emplace<NameComponent>(hostPlayer, NameComponent{"Player"});
    world.AddComponent<Transform>(hostPlayer).position = {81.0f, 82.0f, 83.0f};
    world.AddComponent<CharacterControllerComponent>(hostPlayer);

    const EntityID hostTarget = registry.create(static_cast<EntityID>(102));
    registry.emplace<NameComponent>(hostTarget, NameComponent{"Damageable Target"});
    world.AddComponent<Transform>(hostTarget).position = {71.0f, 72.0f, 73.0f};
    world.AddComponent<MeshRenderer>(hostTarget).visible = false;

    EngineContext context;
    context.SetWorld(&world);
    FPSStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(&context));
    mod.ResetRound();

    EXPECT_NEAR(world.GetComponent<Transform>(hostCamera)->position.x, 91.0f, 0.001f);
    EXPECT_NEAR(world.GetComponent<Transform>(hostPlayer)->position.x, 81.0f, 0.001f);
    EXPECT_NEAR(world.GetComponent<Transform>(hostTarget)->position.x, 71.0f, 0.001f);
    EXPECT_FALSE(world.GetComponent<MeshRenderer>(hostTarget)->visible);

    mod.OnUnload();
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(3));
    EXPECT_TRUE(registry.valid(hostCamera));
    EXPECT_TRUE(registry.valid(hostPlayer));
    EXPECT_TRUE(registry.valid(hostTarget));
}

TEST(Templates_FPSStarter_AppendsArenaAroundLowIdHostEntities)
{
    ScopedCurrentPath projectRoot(FPSStarterProjectRoot());
    World world;

    const EntityID hostEntity = world.CreateEntity("Host Sentinel");
    EXPECT_EQ(static_cast<uint32_t>(hostEntity), static_cast<uint32_t>(0));
    world.AddComponent<Transform>(hostEntity).position = {41.0f, 42.0f, 43.0f};

    EngineContext context;
    context.SetWorld(&world);
    FPSStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(&context));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(7));
    EXPECT_TRUE(world.GetRegistry().valid(hostEntity));
    EXPECT_NEAR(world.GetComponent<Transform>(hostEntity)->position.x, 41.0f, 0.001f);

    mod.OnUnload();
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
    EXPECT_TRUE(world.GetRegistry().valid(hostEntity));
    EXPECT_NEAR(world.GetComponent<Transform>(hostEntity)->position.z, 43.0f, 0.001f);
}

TEST(Templates_FPSStarter_FallsBackWhenStartupLacksArenaContract)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-fps-startup-fallback-" + std::to_string(stamp));
    fs::create_directories(root / "Scenes");
    std::ofstream(root / "Startup.sparkscene", std::ios::binary) << R"({"version":1,"entities":[]})";
    fs::copy_file(FPSStarterProjectRoot() / "Scenes" / "Arena.sparkscene", root / "Scenes" / "Arena.sparkscene",
                  fs::copy_options::overwrite_existing);

    {
        ScopedCurrentPath projectRoot(root);
        World world;
        EngineContext context;
        context.SetWorld(&world);

        FPSStarterModule mod;
        EXPECT_TRUE(mod.OnLoad(&context));
        EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(6));
        EXPECT_TRUE(mod.TryFire());
        EXPECT_NEAR(mod.GetTargetState().health, 75.0f, 0.001f);
        mod.OnUnload();
        EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(0));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(Templates_FPSStarter_RejectsHostEntitiesAsMissingSceneContract)
{
    const std::filesystem::path multiplayerArenaRoot = FPSStarterProjectRoot().parent_path() / "MultiplayerArena";
    ScopedCurrentPath projectRoot(multiplayerArenaRoot);
    World world;
    auto& registry = world.GetRegistry();

    const EntityID hostPlayer = registry.create(static_cast<EntityID>(101));
    registry.emplace<NameComponent>(hostPlayer, NameComponent{"Player"});
    world.AddComponent<Transform>(hostPlayer).position = {81.0f, 82.0f, 83.0f};
    world.AddComponent<CharacterControllerComponent>(hostPlayer);

    const EntityID hostTarget = registry.create(static_cast<EntityID>(102));
    registry.emplace<NameComponent>(hostTarget, NameComponent{"Damageable Target"});
    world.AddComponent<Transform>(hostTarget).position = {71.0f, 72.0f, 73.0f};
    world.AddComponent<MeshRenderer>(hostTarget).visible = false;

    EngineContext context;
    context.SetWorld(&world);
    FPSStarterModule mod;
    EXPECT_FALSE(mod.OnLoad(&context));
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(2));
    EXPECT_TRUE(registry.valid(hostPlayer));
    EXPECT_TRUE(registry.valid(hostTarget));
    EXPECT_NEAR(world.GetComponent<Transform>(hostPlayer)->position.x, 81.0f, 0.001f);
    EXPECT_NEAR(world.GetComponent<Transform>(hostTarget)->position.x, 71.0f, 0.001f);
    EXPECT_FALSE(world.GetComponent<MeshRenderer>(hostTarget)->visible);
    mod.OnUnload();
}

TEST(Templates_FPSStarter_DeadPlayersIgnorePoseInputUntilRespawn)
{
    ScopedCurrentPath projectRoot(FPSStarterProjectRoot());
    World world;
    InputManager input;
    EngineContext context;
    context.SetWorld(&world);
    context.SetInput(&input);

    FPSStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(&context));
    EntityID player = entt::null;
    for (EntityID entity : world.GetEntitiesWith<NameComponent>())
    {
        const NameComponent* named = world.GetComponent<NameComponent>(entity);
        if (named && named->name == "Player")
        {
            player = entity;
            break;
        }
    }
    EXPECT_TRUE(player != entt::null);
    if (player == entt::null)
    {
        mod.OnUnload();
        return;
    }
    Transform* playerTransform = world.GetComponent<Transform>(player);
    EXPECT_TRUE(playerTransform != nullptr);
    if (!playerTransform)
    {
        mod.OnUnload();
        return;
    }
    playerTransform->rotation.y = 37.0f;
    const DirectX::XMFLOAT3 deadPosition = playerTransform->position;

    mod.DamagePlayer(100.0f);
    input.HandleMessage(WM_KEYDOWN, 'W', 0);
    mod.OnUpdate(0.1f);
    input.HandleMessage(WM_KEYUP, 'W', 0);

    EXPECT_FALSE(mod.GetPlayerState().alive);
    EXPECT_NEAR(playerTransform->position.x, deadPosition.x, 0.001f);
    EXPECT_NEAR(playerTransform->position.z, deadPosition.z, 0.001f);
    EXPECT_NEAR(playerTransform->rotation.y, 37.0f, 0.001f);
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
