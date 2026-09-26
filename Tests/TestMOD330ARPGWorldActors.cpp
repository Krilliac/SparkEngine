/**
 * @file TestMOD330ARPGWorldActors.cpp
 * @brief MOD-330: the ARPG hero, its monsters and the dungeon props exist as ECS World entities with meshes
 *
 * Drives the real SparkGameARPG sources through an engine context that exposes a World (as the module sees it
 * at runtime): ARPGDungeonSystem places the crypt kit, including the ModuleKits ARPG landmarks, and
 * ARPGActorPresentation mirrors the hero and every live monster into Transform + MeshRenderer +
 * HealthComponent entities. The scripted fight uses BasicAttack/UsePrimarySkill, the entry points the
 * keyboard input and console commands call, and a real SaveSystem world save/load for the reload case.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameARPG/Source/Combat/ARPGCombatSystem.h"
#include "../GameModules/SparkGameARPG/Source/Demo/ARPGActorPresentation.h"
#include "../GameModules/SparkGameARPG/Source/Demo/ARPGDemoEncounter.h"
#include "../GameModules/SparkGameARPG/Source/Dungeon/ARPGDungeonSystem.h"
#include "../GameModules/SparkGameARPG/Source/Hero/ARPGHeroSystem.h"
#include "../GameModules/SparkGameARPG/Source/Loot/ARPGLootSystem.h"
#include "../GameModules/SparkGameARPG/Source/Monster/ARPGMonsterSystem.h"
#include "../GameModules/SparkGameARPG/Source/Skill/ARPGSkillSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Systems/ECSystems.h"
#include "Engine/SaveSystem/SaveSystem.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace ARPG;

namespace
{
    /// Engine context exposing only a World and the SaveSystem, as the module sees them.
    class ARPGWorldContext final : public Spark::IEngineContext
    {
      public:
        explicit ARPGWorldContext(::World* world) : m_world(world) {}

        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        ::World* GetWorld() override { return m_world; }
        const ::World* GetWorld() const override { return m_world; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }

      private:
        ::World* m_world = nullptr;
    };

    /// The module's systems wired as Main.cpp wires them, against a real World.
    struct ARPGWorldRun
    {
        World world;
        ARPGWorldContext context{&world};
        ARPGHeroSystem heroes;
        ARPGCombatSystem combat;
        ARPGLootSystem loot;
        ARPGDungeonSystem dungeon;
        ARPGSkillSystem skills;
        ARPGMonsterSystem monsters;
        ARPGDemoEncounter encounter;
        ARPGActorPresentation actors;
        bool initialized = false;

        ARPGWorldRun()
        {
            heroes.Initialize(&context);
            combat.Initialize(&context);
            loot.Initialize(&context);
            dungeon.Initialize(&context);
            skills.Initialize(&context, &heroes);
            monsters.Initialize(&context);
            initialized = encounter.Initialize(&heroes, &combat, &loot, &dungeon, &skills, &monsters) &&
                          actors.Initialize(&context, &encounter, &monsters);
        }

        ~ARPGWorldRun()
        {
            actors.Shutdown();
            encounter.Shutdown();
            monsters.Shutdown();
            skills.Shutdown();
            dungeon.Shutdown();
            loot.Shutdown();
            combat.Shutdown();
            heroes.Shutdown();
        }

        ARPGWorldRun(const ARPGWorldRun&) = delete;
        ARPGWorldRun& operator=(const ARPGWorldRun&) = delete;

        /// One module frame: act, tick the systems in OnUpdate order, then sync the World.
        bool Step()
        {
            const bool acted = encounter.UsePrimarySkill() || encounter.BasicAttack();
            skills.Update(0.25f);
            encounter.Update();
            monsters.Update(0.25f);
            actors.SyncActors();
            return acted;
        }

        EntityID Entity(uint32_t id) const { return static_cast<EntityID>(id); }

        bool IsLive(uint32_t id) const { return world.GetRegistry().valid(Entity(id)); }

        std::string NameOf(uint32_t id) const
        {
            const NameComponent* name = world.GetComponent<NameComponent>(Entity(id));
            return name ? name->name : std::string{};
        }

        std::string MeshOf(uint32_t id) const
        {
            const MeshRenderer* renderer = world.GetComponent<MeshRenderer>(Entity(id));
            return renderer ? renderer->meshPath : std::string{};
        }

        size_t CountNamed(std::string_view prefix) const
        {
            size_t count = 0;
            for (const EntityID entity : world.GetRegistry().view<NameComponent>())
                count += std::string_view(world.GetComponent<NameComponent>(entity)->name).starts_with(prefix) ? 1 : 0;
            return count;
        }

        /// Every active monster has exactly one live actor whose health and mesh match it.
        void ExpectMonsterActorsMatch() const
        {
            EXPECT_EQ(actors.GetMonsterActorCount(), monsters.GetActiveMonsterCount());
            EXPECT_EQ(CountNamed(ARPGActorPresentation::MonsterEntityPrefix), monsters.GetActiveMonsterCount());
            for (const MonsterData& monster : monsters.GetActiveMonsters())
            {
                const std::optional<uint32_t> actor = actors.GetMonsterEntity(monster.monsterId);
                ASSERT_TRUE(actor.has_value());
                ASSERT_TRUE(IsLive(*actor));
                EXPECT_EQ(NameOf(*actor), std::string(ARPGActorPresentation::MonsterEntityPrefix) + monster.name);
                EXPECT_EQ(MeshOf(*actor), std::string(ARPGActorPresentation::ActorMeshPath));
                const HealthComponent* health = world.GetComponent<HealthComponent>(Entity(*actor));
                ASSERT_TRUE(health != nullptr);
                EXPECT_EQ(health->health, monster.health);
                EXPECT_EQ(health->maxHealth, monster.maxHealth);
            }
        }
    };

    std::filesystem::path RepoPath(const std::string& relative)
    {
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR) / relative;
    }

    /// Temporary SaveSystem root that restores the singleton's save directory afterwards.
    class ScopedARPGWorldSaveDirectory
    {
      public:
        ScopedARPGWorldSaveDirectory()
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() / "spark_mod330_world_actors")
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
            std::filesystem::create_directories(m_directory, error);
            m_saveSystem.SetFileCache(nullptr);
            m_initialized = m_saveSystem.Initialize(m_directory.string());
        }

        ~ScopedARPGWorldSaveDirectory()
        {
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        ScopedARPGWorldSaveDirectory(const ScopedARPGWorldSaveDirectory&) = delete;
        ScopedARPGWorldSaveDirectory& operator=(const ScopedARPGWorldSaveDirectory&) = delete;

        bool IsInitialized() const { return m_initialized; }
        Spark::SaveSystem& System() { return m_saveSystem; }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        bool m_initialized = false;
    };

    constexpr const char* WorldDemoStateKey = "SparkGameARPG.demo.v1";
    constexpr size_t CryptKitPropCount = 11;
} // namespace

TEST(ARPGDungeon_WorldHoldsHeroMonsterAndModuleKitProps)
{
    ARPGWorldRun run;
    ASSERT_TRUE(run.initialized);

    // Hero: one actor at the crypt entrance, facing into the dungeon, carrying the hero's health.
    const std::optional<uint32_t> heroActor = run.actors.GetHeroEntity();
    ASSERT_TRUE(heroActor.has_value());
    ASSERT_TRUE(run.IsLive(*heroActor));
    EXPECT_EQ(run.NameOf(*heroActor), std::string(ARPGActorPresentation::HeroEntityName));
    EXPECT_EQ(run.MeshOf(*heroActor), std::string(ARPGActorPresentation::ActorMeshPath));
    const Transform* heroTransform = run.world.GetComponent<Transform>(run.Entity(*heroActor));
    ASSERT_TRUE(heroTransform != nullptr);
    EXPECT_EQ(heroTransform->position.z, 0.0f);
    EXPECT_EQ(heroTransform->rotation.y, 180.0f);
    const HealthComponent* heroHealth = run.world.GetComponent<HealthComponent>(run.Entity(*heroActor));
    ASSERT_TRUE(heroHealth != nullptr);
    EXPECT_EQ(heroHealth->health, run.encounter.GetHero()->health);
    EXPECT_EQ(heroHealth->maxHealth, run.encounter.GetHero()->maxHealth);

    // The encounter's first target stands in the aisle in front of the hero.
    ASSERT_EQ(run.monsters.GetActiveMonsterCount(), 1u);
    run.ExpectMonsterActorsMatch();
    const std::optional<uint32_t> targetActor = run.actors.GetMonsterEntity(run.encounter.GetState().targetMonsterId);
    ASSERT_TRUE(targetActor.has_value());
    EXPECT_LT(run.world.GetComponent<Transform>(run.Entity(*targetActor))->position.z, 0.0f);

    // Dungeon props: the ARPG Kit and the ModuleKits ARPG landmarks, each a meshed entity whose mesh is on disk.
    const std::vector<uint32_t>& kit = run.dungeon.GetCryptKitEntities();
    ASSERT_EQ(kit.size(), CryptKitPropCount);
    EXPECT_EQ(run.CountNamed(ARPGDungeonSystem::CRYPT_PROP_PREFIX), CryptKitPropCount);
    std::set<std::string> kitMeshes;
    for (const uint32_t prop : kit)
    {
        ASSERT_TRUE(run.IsLive(prop));
        ASSERT_TRUE(run.world.GetComponent<Transform>(run.Entity(prop)) != nullptr);
        const std::string mesh = run.MeshOf(prop);
        EXPECT_TRUE(std::filesystem::is_regular_file(RepoPath(mesh)));
        kitMeshes.insert(mesh);
    }
    EXPECT_TRUE(kitMeshes.contains("Assets/Models/ModuleKits/ARPG/necrotic_combat_pillar.obj"));
    EXPECT_TRUE(kitMeshes.contains("Assets/Models/ModuleKits/ARPG/summoner_ritual_brazier.obj"));
    EXPECT_TRUE(kitMeshes.contains("Assets/Models/ModuleKits/ARPG/arcane_loot_chest.obj"));
    EXPECT_TRUE(kitMeshes.contains("Assets/Models/ARPG/Kit/portal_gate.obj"));
    EXPECT_TRUE(std::filesystem::is_regular_file(RepoPath(ARPGActorPresentation::ActorMeshPath)));

    // World holds exactly the kit, the hero and the one target.
    EXPECT_EQ(run.world.GetEntityCount(), CryptKitPropCount + 2u);
}

TEST(ARPGDungeon_WorldActorsFollowCombatKillsAndBoss)
{
    ARPGWorldRun run;
    ASSERT_TRUE(run.initialized);

    uint32_t previousTarget = run.encounter.GetState().targetMonsterId;
    std::optional<uint32_t> previousActor = run.actors.GetMonsterEntity(previousTarget);
    ASSERT_TRUE(previousActor.has_value());
    uint32_t killsSeen = 0;
    bool sawBossActor = false;
    for (int frame = 0; frame < 50000 && !run.encounter.IsRunComplete(); ++frame)
    {
        ASSERT_TRUE(run.Step());
        run.ExpectMonsterActorsMatch();

        const uint32_t target = run.encounter.GetState().targetMonsterId;
        if (target != previousTarget)
        {
            // The killed monster's actor left the World with it.
            EXPECT_FALSE(run.IsLive(*previousActor));
            ++killsSeen;
            previousTarget = target;
            previousActor = run.actors.GetMonsterEntity(target);
        }

        const MonsterData* targetData = run.encounter.GetTarget();
        if (targetData && targetData->rank == ARPGMonsterRank::Boss)
        {
            // The boss holds the arena between the necrotic pillars and is drawn larger.
            const std::optional<uint32_t> bossActor = run.actors.GetMonsterEntity(targetData->monsterId);
            ASSERT_TRUE(bossActor.has_value());
            const Transform* transform = run.world.GetComponent<Transform>(run.Entity(*bossActor));
            ASSERT_TRUE(transform != nullptr);
            EXPECT_EQ(transform->position.z, -10.5f);
            EXPECT_GT(transform->scale.y, 1.0f);
            sawBossActor = true;
        }
    }
    ASSERT_TRUE(run.encounter.IsRunComplete());
    EXPECT_TRUE(sawBossActor);
    EXPECT_EQ(killsSeen, run.encounter.GetState().totalKills);

    // Cleared run: no monster actors remain, the hero and the kit stay.
    EXPECT_EQ(run.actors.GetMonsterActorCount(), 0u);
    EXPECT_EQ(run.CountNamed(ARPGActorPresentation::MonsterEntityPrefix), 0u);
    EXPECT_EQ(run.world.GetEntityCount(), CryptKitPropCount + 1u);

    // Restart spawns a fresh target and its actor.
    run.encounter.Restart();
    run.actors.SyncActors();
    EXPECT_EQ(run.monsters.GetActiveMonsterCount(), 1u);
    run.ExpectMonsterActorsMatch();

    // Shutdown removes every entity the ARPG systems placed.
    run.actors.Shutdown();
    run.dungeon.Shutdown();
    EXPECT_EQ(run.world.GetEntityCount(), 0u);
}

TEST(ARPGDungeon_WorldLoadRebuildsActorsAndKitWithoutDuplicates)
{
    ScopedARPGWorldSaveDirectory saves;
    ASSERT_TRUE(saves.IsInitialized());

    ARPGWorldRun run;
    ASSERT_TRUE(run.initialized);
    for (int frame = 0; frame < 3; ++frame)
        ASSERT_TRUE(run.Step());

    // arpg_save: the whole World (kit and actors included) plus the encounter snapshot.
    const std::string snapshot = run.encounter.SerializeState();
    ASSERT_FALSE(snapshot.empty());
    Spark::SaveMetadata meta;
    meta.saveName = "ARPG MOD-330 world actors";
    ASSERT_TRUE(saves.System().Save("arpg_world", run.world, meta, {{WorldDemoStateKey, snapshot}}));

    // Keep playing so the live World differs from the saved one (new targets, kills, removed actors).
    for (int frame = 0; frame < 40; ++frame)
        ASSERT_TRUE(run.Step());

    // arpg_load: SaveSystem replaces every World entity, then the module restores and rebuilds.
    std::unordered_map<std::string, std::string> loadedState;
    ASSERT_TRUE(saves.System().Load("arpg_world", run.world, loadedState));
    ASSERT_TRUE(loadedState.contains(WorldDemoStateKey));
    ASSERT_TRUE(run.encounter.RestoreState(loadedState.at(WorldDemoStateKey)));
    run.dungeon.RebuildCryptKitAfterWorldLoad();
    run.actors.RebuildAfterWorldLoad();

    // Exactly one hero, one actor per live monster and one copy of the kit: no restored duplicates survive and
    // every cached identifier names the entity it claims to.
    EXPECT_EQ(run.CountNamed(ARPGActorPresentation::HeroEntityName), 1u);
    const std::optional<uint32_t> heroActor = run.actors.GetHeroEntity();
    ASSERT_TRUE(heroActor.has_value());
    EXPECT_EQ(run.NameOf(*heroActor), std::string(ARPGActorPresentation::HeroEntityName));
    EXPECT_EQ(run.world.GetComponent<HealthComponent>(run.Entity(*heroActor))->health, run.encounter.GetHero()->health);
    run.ExpectMonsterActorsMatch();
    EXPECT_EQ(run.monsters.GetActiveMonsterCount(), 1u);

    const std::vector<uint32_t>& kit = run.dungeon.GetCryptKitEntities();
    ASSERT_EQ(kit.size(), CryptKitPropCount);
    EXPECT_EQ(run.CountNamed(ARPGDungeonSystem::CRYPT_PROP_PREFIX), CryptKitPropCount);
    for (const uint32_t prop : kit)
    {
        ASSERT_TRUE(run.IsLive(prop));
        EXPECT_TRUE(run.NameOf(prop).starts_with(ARPGDungeonSystem::CRYPT_PROP_PREFIX));
        EXPECT_FALSE(run.MeshOf(prop).empty());
    }
    EXPECT_EQ(run.world.GetEntityCount(), CryptKitPropCount + 2u);

    // Teardown through the rebuilt identifiers leaves nothing behind.
    run.actors.Shutdown();
    run.dungeon.Shutdown();
    EXPECT_EQ(run.world.GetEntityCount(), 0u);
}

TEST(ARPGDungeon_MirroredDeathsDoNotReachEngineLifecycle)
{
    ARPGWorldRun run;
    ASSERT_TRUE(run.initialized);

    // Kill the hero and the current target in the gameplay systems, then mirror them into the World.
    const HeroData* hero = run.encounter.GetHero();
    ASSERT_TRUE(hero != nullptr);
    run.heroes.GetHero(hero->heroId)->health = 0.0f;
    const uint32_t targetId = run.encounter.GetState().targetMonsterId;
    MonsterData* target = run.monsters.GetMonster(targetId);
    ASSERT_TRUE(target != nullptr);
    target->health = 0.0f;
    run.actors.SyncActors();

    const std::optional<uint32_t> heroActor = run.actors.GetHeroEntity();
    const std::optional<uint32_t> targetActor = run.actors.GetMonsterEntity(targetId);
    ASSERT_TRUE(heroActor.has_value());
    ASSERT_TRUE(targetActor.has_value());
    for (const uint32_t actor : {*heroActor, *targetActor})
    {
        const HealthComponent* health = run.world.GetComponent<HealthComponent>(run.Entity(actor));
        ASSERT_TRUE(health != nullptr);
        EXPECT_TRUE(health->isDead);
        EXPECT_TRUE(health->deathProcessed);
    }

    // The gameplay systems own these deaths: the engine LifecycleSystem must not fire for the mirrors.
    Spark::ECS::LifecycleSystem lifecycle;
    uint32_t engineDeaths = 0;
    lifecycle.SetDeathCallback([&engineDeaths](EntityID) { ++engineDeaths; });
    lifecycle.Update(run.world, 0.016f);
    EXPECT_EQ(engineDeaths, 0u);

    // A revived hero clears both flags so a later death is mirrored again.
    run.heroes.GetHero(hero->heroId)->health = hero->maxHealth;
    run.actors.SyncActors();
    const HealthComponent* revived = run.world.GetComponent<HealthComponent>(run.Entity(*heroActor));
    ASSERT_TRUE(revived != nullptr);
    EXPECT_FALSE(revived->isDead);
    EXPECT_FALSE(revived->deathProcessed);
}

#endif // SPARK_TEST_HAS_IMGUI
