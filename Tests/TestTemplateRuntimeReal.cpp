/**
 * @file TestTemplateRuntimeReal.cpp
 * @brief Real-behavior coverage for the shared template runtime bridge and the
 *        template-side contracts that depend on it.
 *
 * Exercises Spark::Templates::TemplateRuntimeScene and the shipped template
 * modules through their production headers:
 *  - the deterministic OnLoad(nullptr) seam is distinguishable from a real load;
 *  - a scene contract fails loudly when a required entity is gone and only warns
 *    when a decorative prop is gone;
 *  - Blank3D's public Move() integrates along the same yaw the live input does;
 *  - RPGStarter's advertised save/load actually reaches disk and is readable by a
 *    second module instance.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Game/TemplateRuntime.h"
#include "SceneManager/ReflectedSceneSerializer.h"

#include "../Templates/Blank3D/Source/GameModule.h"
#include "../Templates/RPGStarter/Source/GameModule.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    class ScopedRuntimeTestDirectory
    {
      public:
        explicit ScopedRuntimeTestDirectory(const std::string& label)
            : m_previous(std::filesystem::current_path()),
              m_root(std::filesystem::temp_directory_path() /
                     ("spark-template-runtime-" + label + "-" +
                      std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count())))
        {
            std::filesystem::create_directories(m_root / "Scenes");
            std::filesystem::current_path(m_root);
        }

        ~ScopedRuntimeTestDirectory()
        {
            std::error_code ec;
            std::filesystem::current_path(m_previous, ec);
            std::filesystem::remove_all(m_root, ec);
        }

        ScopedRuntimeTestDirectory(const ScopedRuntimeTestDirectory&) = delete;
        ScopedRuntimeTestDirectory& operator=(const ScopedRuntimeTestDirectory&) = delete;

        [[nodiscard]] const std::filesystem::path& GetRoot() const { return m_root; }

      private:
        std::filesystem::path m_previous;
        std::filesystem::path m_root;
    };

    std::filesystem::path TemplatePackageRoot(const char* package)
    {
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Templates" / package;
    }

    /** Copy one authored template scene into the scratch project, optionally dropping an entity. */
    bool StageSceneWithout(const std::filesystem::path& authoredScene, const std::filesystem::path& destination,
                           const char* removedEntityName)
    {
        if (removedEntityName == nullptr)
        {
            std::error_code ec;
            std::filesystem::copy_file(authoredScene, destination, std::filesystem::copy_options::overwrite_existing,
                                       ec);
            return !ec;
        }

        World staged;
        if (!Spark::LoadWorld(staged, authoredScene.string()))
            return false;

        {
            bool removed = false;
            auto& storage = staged.GetRegistry().storage<entt::entity>();
            for (auto&& [entity] : storage.each())
            {
                const NameComponent* named = staged.GetComponent<NameComponent>(entity);
                if (named && named->name == removedEntityName)
                {
                    staged.DestroyEntity(entity);
                    removed = true;
                    break;
                }
            }
            if (!removed)
                return false;
        }
        return Spark::SaveWorld(staged, destination.string());
    }
} // namespace

TEST(TemplateRuntime_DeterministicSeamIsDistinguishableFromLoadedScene)
{
    const auto acceptEverything = [](const Spark::Templates::TemplateRuntimeScene&) { return true; };

    // No context: the seam returns true without resolving a scene. A caller that
    // reads only the bool cannot tell this from a successful load, which is why
    // LastLoadResult() exists.
    {
        Spark::Templates::TemplateRuntimeScene runtime;
        EXPECT_TRUE(runtime.Load(nullptr, "SeamProbe", {"Scenes/Default.sparkscene"}, acceptEverything));
        EXPECT_FALSE(runtime.IsActive());
        EXPECT_TRUE(runtime.LastLoadResult() == Spark::Templates::TemplateLoadResult::Deterministic);
        EXPECT_TRUE(runtime.GetWorld() == nullptr);
    }

    ScopedRuntimeTestDirectory project("seam");
    EXPECT_TRUE(StageSceneWithout(TemplatePackageRoot("Blank3D") / "Scenes" / "Default.sparkscene",
                                  project.GetRoot() / "Scenes" / "Default.sparkscene", nullptr));

    World world;
    const EntityID hostEntity = world.CreateEntity("Host Sentinel");
    EngineContext context;
    context.SetWorld(&world);

    {
        Spark::Templates::TemplateRuntimeScene runtime;
        EXPECT_TRUE(runtime.Load(&context, "SeamProbe", {"Scenes/Default.sparkscene"}, acceptEverything));
        EXPECT_TRUE(runtime.IsActive());
        EXPECT_TRUE(runtime.LastLoadResult() == Spark::Templates::TemplateLoadResult::Loaded);
        EXPECT_TRUE(world.GetEntityCount() > static_cast<size_t>(1));
        EXPECT_TRUE(runtime.Find("Main Camera") != Spark::Templates::TemplateRuntimeScene::InvalidEntity);

        // A contract that never accepts a candidate is a failure, not a seam.
        const auto rejectEverything = [](const Spark::Templates::TemplateRuntimeScene&) { return false; };
        EXPECT_FALSE(runtime.Load(&context, "SeamProbe", {"Scenes/Default.sparkscene"}, rejectEverything));
        EXPECT_FALSE(runtime.IsActive());
        EXPECT_TRUE(runtime.LastLoadResult() == Spark::Templates::TemplateLoadResult::Failed);
    }

    // Only the runtime's own entities are removed on a failed load.
    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
    EXPECT_TRUE(world.GetRegistry().valid(hostEntity));
}

TEST(TemplateRuntime_UnloadRemovesOnlyOwnedEntities)
{
    ScopedRuntimeTestDirectory project("ownership");
    EXPECT_TRUE(StageSceneWithout(TemplatePackageRoot("Blank3D") / "Scenes" / "Default.sparkscene",
                                  project.GetRoot() / "Scenes" / "Default.sparkscene", nullptr));

    World world;
    const EntityID hostEntity = world.CreateEntity("Host Sentinel");
    world.AddComponent<Transform>(hostEntity).position = {7.0f, 8.0f, 9.0f};
    EngineContext context;
    context.SetWorld(&world);

    {
        Spark::Templates::TemplateRuntimeScene runtime;
        EXPECT_TRUE(runtime.Load(&context, "OwnershipProbe", {"Scenes/Default.sparkscene"},
                                 [](const Spark::Templates::TemplateRuntimeScene&) { return true; }));
        EXPECT_TRUE(world.GetEntityCount() > static_cast<size_t>(1));
        runtime.Unload();
        EXPECT_TRUE(runtime.LastLoadResult() == Spark::Templates::TemplateLoadResult::Deterministic);
    }

    EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(1));
    EXPECT_TRUE(world.GetRegistry().valid(hostEntity));
    EXPECT_NEAR(world.GetComponent<Transform>(hostEntity)->position.x, 7.0f, 0.001f);
}

TEST(Templates_Blank3D_LoadsWithoutADecorativePropButNotWithoutItsCamera)
{
    // Editing the shipped scene is the first thing a user does. Removing a
    // composition prop must degrade the preview, not refuse the module.
    {
        ScopedRuntimeTestDirectory project("blank3d-prop");
        EXPECT_TRUE(StageSceneWithout(TemplatePackageRoot("Blank3D") / "Scenes" / "Default.sparkscene",
                                      project.GetRoot() / "Scenes" / "Default.sparkscene", "Starter Cube"));

        World world;
        EngineContext context;
        context.SetWorld(&world);

        Blank3DModule mod;
        EXPECT_TRUE(mod.OnLoad(&context));
        EXPECT_TRUE(world.GetEntityCount() > static_cast<size_t>(0));
        mod.OnUnload();
        EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(0));
    }

    // The camera the module renders through stays required.
    {
        ScopedRuntimeTestDirectory project("blank3d-camera");
        EXPECT_TRUE(StageSceneWithout(TemplatePackageRoot("Blank3D") / "Scenes" / "Default.sparkscene",
                                      project.GetRoot() / "Scenes" / "Default.sparkscene", "Main Camera"));

        World world;
        EngineContext context;
        context.SetWorld(&world);

        Blank3DModule mod;
        EXPECT_FALSE(mod.OnLoad(&context));
        EXPECT_EQ(world.GetEntityCount(), static_cast<size_t>(0));
    }
}

TEST(Templates_Blank3D_PublicMoveUsesTheSameYawAsLiveInput)
{
    Blank3DModule keyboard;
    EXPECT_TRUE(keyboard.OnLoad(nullptr));
    keyboard.ResetCamera();
    keyboard.Look(105.0f, 0.0f); // -15 authored yaw + 105 = +90 degrees.
    const Blank3DCameraState before = keyboard.GetCameraState();
    keyboard.Move(1.0f, 0.0f, 0.0f, 1.0f);
    const Blank3DCameraState after = keyboard.GetCameraState();

    // Facing +X, forward must move x and leave z alone. World-axis motion would
    // have moved z by the full move speed and left x untouched.
    EXPECT_NEAR(after.x, before.x + before.moveSpeed, 0.001f);
    EXPECT_NEAR(after.z, before.z, 0.001f);
    keyboard.OnUnload();
}

TEST(Templates_RPGStarter_SaveSlotSurvivesTheModuleInstance)
{
    ScopedRuntimeTestDirectory project("rpg-save");
    EXPECT_TRUE(StageSceneWithout(TemplatePackageRoot("RPGStarter") / "Scenes" / "Village.sparkscene",
                                  project.GetRoot() / "Scenes" / "Village.sparkscene", nullptr));

    const std::filesystem::path slot = project.GetRoot() / "Saves" / "rpg_slot0.spark_save";
    uint32_t savedGold = 0;

    {
        World world;
        EngineContext context;
        context.SetWorld(&world);

        RPGStarterModule mod;
        EXPECT_TRUE(mod.OnLoad(&context));
        mod.TalkToElder();
        mod.CloseDialogue();
        EXPECT_TRUE(mod.PickUpRelic());
        EXPECT_TRUE(mod.AttackWarden());
        EXPECT_TRUE(mod.AttackWarden());
        EXPECT_TRUE(mod.AttackWarden());
        mod.TalkToElder();
        EXPECT_TRUE(mod.ClaimReward());
        EXPECT_TRUE(mod.SaveToSlot());
        savedGold = mod.GetState().gold;
        EXPECT_EQ(savedGold, static_cast<uint32_t>(50));
        EXPECT_TRUE(std::filesystem::is_regular_file(slot));
        EXPECT_TRUE(mod.HasSave());
        EXPECT_TRUE(mod.HasDiskSave());
        mod.OnUnload();
    }

    // A second module instance is what a relaunched game looks like: nothing is
    // in memory, so the slot has to come back off disk.
    {
        World world;
        EngineContext context;
        context.SetWorld(&world);

        RPGStarterModule relaunched;
        EXPECT_TRUE(relaunched.OnLoad(&context));
        // The save HUD reads this: a slot left by the previous session has to be
        // visible from the very first frame, without a per-frame stat.
        EXPECT_TRUE(relaunched.HasDiskSave());
        EXPECT_TRUE(relaunched.HasSave());
        EXPECT_TRUE(relaunched.LoadFromSlot());
        EXPECT_TRUE(relaunched.HasItem("Lost Relic"));
        EXPECT_EQ(relaunched.GetState().gold, savedGold);
        EXPECT_TRUE(relaunched.IsRewardClaimed());
        EXPECT_NEAR(relaunched.GetState().enemyHealth, 0.0f, 0.001f);
        relaunched.OnUnload();
    }
}

TEST(Templates_RPGStarter_DeterministicSeamKeepsTheSlotInProcess)
{
    // Without a project root there is nowhere to persist to, and SaveToSlot must
    // say so instead of reporting a save that never reached disk.
    RPGStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(nullptr));
    EXPECT_TRUE(mod.GetSaveFilePath().empty());
    EXPECT_FALSE(mod.SaveToSlot());
    EXPECT_TRUE(mod.HasSave());
    // The in-memory snapshot must not be advertised as a slot that survives exit:
    // this is exactly what the save HUD cell reports.
    EXPECT_FALSE(mod.HasDiskSave());
    mod.NewGame();
    EXPECT_TRUE(mod.LoadFromSlot());
    mod.OnUnload();
}

TEST(Templates_RPGStarter_RefusesASlotWithNonFiniteFields)
{
    ScopedRuntimeTestDirectory project("rpg-save-guard");
    EXPECT_TRUE(StageSceneWithout(TemplatePackageRoot("RPGStarter") / "Scenes" / "Village.sparkscene",
                                  project.GetRoot() / "Scenes" / "Village.sparkscene", nullptr));

    // Every float in the slot is untrusted file text. A hand-edited yaw, enemy
    // health or cooldown of nan/inf must be refused outright instead of reaching
    // the live entity transforms, where a non-finite rotation poisons the run.
    std::filesystem::create_directories(project.GetRoot() / "Saves");
    {
        std::ofstream slot(project.GetRoot() / "Saves" / "rpg_slot0.spark_save", std::ios::binary | std::ios::trunc);
        slot << "version=1\nx=1\nz=2\nhealth=80\nenemyHealth=nan\ngold=5\nexperience=0\n"
                "questStage=2\ndialogueOpen=0\nenemyDefeated=0\nrewardClaimed=0\n"
                "heroYaw=nan\nwardenYaw=-inf\nenemyAttackCooldown=-3\n";
    }

    World world;
    EngineContext context;
    context.SetWorld(&world);

    RPGStarterModule mod;
    EXPECT_TRUE(mod.OnLoad(&context));
    EXPECT_FALSE(mod.LoadFromSlot());
    EXPECT_TRUE(std::isfinite(mod.GetState().enemyHealth));
    EXPECT_TRUE(std::isfinite(mod.GetHeroYawDegrees()));
    EXPECT_TRUE(std::isfinite(mod.GetWardenYawDegrees()));
    EXPECT_TRUE(mod.GetEnemyAttackCooldown() >= 0.0f);
    mod.OnUnload();
}
