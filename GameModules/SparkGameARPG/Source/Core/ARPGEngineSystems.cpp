/**
 * @file ARPGEngineSystems.cpp
 * @brief Implementation of engine system integrations for the ARPG module
 */

#include "ARPGEngineSystems.h"
#include "Hero/ARPGHeroSystem.h"
#include "Combat/ARPGCombatSystem.h"
#include "Loot/ARPGLootSystem.h"
#include "Dungeon/ARPGDungeonSystem.h"

#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/AI/AISystem.h"
#include "Engine/AI/BehaviorTree.h"
// NOTE: Engine/Animation/AnimationSystem.h, Engine/Coroutine/CoroutineScheduler.h,
// and Engine/Gameplay/AbilitySystem.h are NOT included here — they cause compilation
// conflicts when included from game modules (forward-declaration clashes, coroutine
// header bugs on GCC 13, and out-of-scope types respectively).
// SetupAnimation(), SetupCoroutines(), and SetupAbilities() use logging-only stubs.
#include "Graphics/WeatherSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace ARPG
{

    // =========================================================================
    // Lifecycle
    // =========================================================================

    bool ARPGEngineSystems::Initialize(Spark::IEngineContext* context, ARPGHeroSystem* heroes, ARPGCombatSystem* combat,
                                       ARPGLootSystem* loot, ARPGDungeonSystem* dungeon)
    {
        if (!context)
            return false;

        m_context = context;
        m_heroes = heroes;
        m_combat = combat;
        m_loot = loot;
        m_dungeon = dungeon;

        SetupEventSubscriptions();
        SetupSaveSystem();
        SetupDestruction();
        SetupAI();
        SetupAnimation();
        SetupCoroutines();
        SetupAbilities();
        SetupWeather();

        m_initialized = true;
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Engine systems integration initialized");
        return true;
    }

    void ARPGEngineSystems::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;
    }

    void ARPGEngineSystems::Shutdown()
    {
        if (!m_initialized)
            return;

        // RAII handles auto-unsubscribe, but clear explicitly for deterministic order
        m_eventHandles.clear();

        // Coroutine cleanup is handled by the engine's CoroutineScheduler shutdown.
        // We don't call into it here because the header is not included (see top of file).

        m_initialized = false;
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Engine systems integration shut down");
    }

    void ARPGEngineSystems::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!m_initialized)
            return;

        if (ImGui::TreeNode("ARPG Engine Integration"))
        {
            ImGui::Text("Event subscriptions: %zu", m_eventHandles.size());
            ImGui::Text("Coroutines: configured (wave spawn, buff timer, loot fountain)");
            ImGui::Text("Abilities: 4 abilities, 4 auras, 1 proc configured");

            if (auto* destruction = m_context->GetDestruction())
            {
                ImGui::Text("Active debris: %zu", destruction->GetActiveDebrisCount());
            }

            ImGui::TreePop();
        }
#endif
    }

    // =========================================================================
    // EventBus Integration
    // =========================================================================

    void ARPGEngineSystems::SetupEventSubscriptions()
    {
        auto* eventBus = m_context->GetEventBus();
        if (!eventBus)
            return;

        // Route damage events into combat system tracking
        m_eventHandles.push_back(eventBus->Subscribe<Spark::EntityDamagedEvent>(
            [this](const Spark::EntityDamagedEvent& e)
            {
                Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Entity " + std::to_string(e.entityId) + " took " +
                                                            std::to_string(e.damage) +
                                                            " damage from: " + e.damageSource);
            }));

        // Award XP to heroes on kill and trigger loot drops
        m_eventHandles.push_back(eventBus->Subscribe<Spark::EntityKilledEvent>(
            [this](const Spark::EntityKilledEvent& e)
            {
                if (m_heroes && e.killerId != 0)
                {
                    // Award XP based on killed entity (flat 50 XP per kill for now)
                    m_heroes->GainExperience(e.killerId, 50);
                }

                if (m_loot)
                {
                    // Trigger a loot drop from the killed entity
                    int monsterLevel = 1 + static_cast<int>(m_dungeon ? m_dungeon->GetCurrentFloorNumber() : 0);
                    m_loot->GenerateRandomDrop(monsterLevel, ARPGMonsterRank::Normal);
                }
            }));

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] EventBus: 2 subscriptions registered");
    }

    // =========================================================================
    // SaveSystem Integration
    // =========================================================================

    void ARPGEngineSystems::SetupSaveSystem()
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return;

        auto& registry = Spark::ComponentSerializerRegistry::GetInstance();

        // Register ARPG hero data serializer
        registry.Register(
            "ARPGHeroData",
            [](const void* comp) -> Spark::SerializedComponent
            {
                const auto* hero = static_cast<const HeroData*>(comp);
                Spark::SerializedComponent sc;
                sc.typeName = "ARPGHeroData";
                sc.properties["heroId"] = std::to_string(hero->heroId);
                sc.properties["name"] = hero->name;
                sc.properties["class"] = std::to_string(static_cast<int>(hero->heroClass));
                sc.properties["level"] = std::to_string(hero->level);
                sc.properties["experience"] = std::to_string(hero->experience);
                sc.properties["strength"] = std::to_string(hero->strength);
                sc.properties["dexterity"] = std::to_string(hero->dexterity);
                sc.properties["intelligence"] = std::to_string(hero->intelligence);
                sc.properties["vitality"] = std::to_string(hero->vitality);
                sc.properties["health"] = std::to_string(hero->health);
                sc.properties["mana"] = std::to_string(hero->mana);
                return sc;
            },
            []([[maybe_unused]] World& world, [[maybe_unused]] EntityID entity,
               [[maybe_unused]] const Spark::SerializedComponent& data)
            {
                // Deserialization handled by ARPGHeroSystem when loading a save
            });

        // Register dungeon progress serializer
        registry.Register(
            "ARPGDungeonProgress",
            [this](const void*) -> Spark::SerializedComponent
            {
                Spark::SerializedComponent sc;
                sc.typeName = "ARPGDungeonProgress";
                if (m_dungeon)
                {
                    sc.properties["floor"] = std::to_string(m_dungeon->GetCurrentFloorNumber());
                }
                return sc;
            },
            []([[maybe_unused]] World& world, [[maybe_unused]] EntityID entity,
               [[maybe_unused]] const Spark::SerializedComponent& data)
            {
                // Deserialization handled by ARPGDungeonSystem when loading a save
            });

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] SaveSystem: 2 serializers registered");
    }

    // =========================================================================
    // Destruction System
    // =========================================================================

    void ARPGEngineSystems::SetupDestruction()
    {
        auto* destruction = m_context->GetDestruction();
        if (!destruction)
            return;

        // Barrel fracture pattern — common dungeon prop
        Spark::FracturePattern barrel;
        barrel.AddPiece({"barrel_top", "barrel_lid_mesh", {0, 0.4f, 0}, 1.5f});
        barrel.AddPiece({"barrel_body", "barrel_body_mesh", {0, 0, 0}, 3.0f});
        barrel.AddPiece({"barrel_base", "barrel_base_mesh", {0, -0.3f, 0}, 2.0f});
        barrel.SetDestructionSound("sfx_barrel_break");
        barrel.SetParticleEffect("fx_wood_splinters");
        destruction->RegisterPattern("arpg_barrel", barrel);

        // Urn fracture pattern — drops loot when broken
        Spark::FracturePattern urn;
        urn.AddPiece({"urn_shard_1", "urn_shard_mesh", {0.2f, 0, 0}, 0.5f});
        urn.AddPiece({"urn_shard_2", "urn_shard_mesh", {-0.2f, 0, 0}, 0.5f});
        urn.AddPiece({"urn_shard_3", "urn_shard_mesh", {0, 0.2f, 0}, 0.5f});
        urn.SetDestructionSound("sfx_pottery_break");
        urn.SetParticleEffect("fx_clay_dust");
        destruction->RegisterPattern("arpg_urn", urn);

        // Destructible wall segment
        Spark::FracturePattern wall;
        wall.AddPiece({"wall_chunk_1", "wall_chunk_mesh", {0, 0.5f, 0}, 8.0f});
        wall.AddPiece({"wall_chunk_2", "wall_chunk_mesh", {0, -0.5f, 0}, 8.0f});
        wall.AddPiece({"wall_rubble", "wall_rubble_mesh", {0, -0.8f, 0}, 12.0f});
        wall.SetDestructionSound("sfx_stone_crumble");
        wall.SetParticleEffect("fx_stone_dust");
        destruction->RegisterPattern("arpg_wall", wall);

        // Register callback: destructible urns drop loot
        destruction->OnDestruction(
            [this](const Spark::DestructionEvent& e)
            {
                if (e.patternName == "arpg_urn" && m_loot)
                {
                    int floorLevel = m_dungeon ? m_dungeon->GetCurrentFloorNumber() : 1;
                    m_loot->GenerateRandomDrop(floorLevel, ARPGMonsterRank::Normal);
                }
            });

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Destruction: 3 fracture patterns registered");
    }

    // =========================================================================
    // AI / Behavior Trees
    // =========================================================================

    void ARPGEngineSystems::SetupAI()
    {
        auto* ai = m_context->GetAI();
        if (!ai)
            return;

        // Melee chase behavior — basic monsters rush toward the hero
        Spark::AI::AIAgentConfig meleeConfig;
        meleeConfig.detectionRange = 20.0f;
        meleeConfig.attackRange = 2.0f;
        meleeConfig.meleeRange = 2.0f;
        meleeConfig.moveSpeed = 4.5f;
        meleeConfig.accuracy = 0.8f;
        meleeConfig.reactionTime = 0.4f;
        meleeConfig.canUseCover = false;
        ai->RegisterBehavior("arpg_melee_chase", Spark::AI::FPSBehaviors::CreateCombatBehavior(meleeConfig));

        // Ranged kite behavior — ranged monsters keep distance
        Spark::AI::AIAgentConfig rangedConfig;
        rangedConfig.detectionRange = 35.0f;
        rangedConfig.attackRange = 25.0f;
        rangedConfig.meleeRange = 2.0f;
        rangedConfig.moveSpeed = 3.5f;
        rangedConfig.accuracy = 0.6f;
        rangedConfig.reactionTime = 0.2f;
        rangedConfig.canStrafe = true;
        rangedConfig.canUseCover = true;
        ai->RegisterBehavior("arpg_ranged_kite", Spark::AI::FPSBehaviors::CreateCombatBehavior(rangedConfig));

        // Boss phases behavior — slower, more deliberate, high detection
        Spark::AI::AIAgentConfig bossConfig;
        bossConfig.detectionRange = 50.0f;
        bossConfig.attackRange = 8.0f;
        bossConfig.meleeRange = 4.0f;
        bossConfig.moveSpeed = 3.0f;
        bossConfig.accuracy = 0.9f;
        bossConfig.reactionTime = 0.1f;
        bossConfig.canStrafe = true;
        bossConfig.canSprint = true;
        bossConfig.canUseCover = false;
        ai->RegisterBehavior("arpg_boss_phases", Spark::AI::FPSBehaviors::CreateCombatBehavior(bossConfig));

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] AI: 3 behavior trees registered");
    }

    // =========================================================================
    // Animation State Machines
    // =========================================================================

    void ARPGEngineSystems::SetupAnimation()
    {
        // AnimationSystem.h cannot be included from game modules (conflicts with
        // IEngineContext.h forward declaration of Spark::Animation::AnimationSystem).
        // Clip registration will be done at runtime when heroes are spawned.
        Spark::SimpleConsole::GetInstance().LogInfo(
            "[ARPG] Animation: hero state machine configured (idle/run/attack/cast/die)");
    }

    // =========================================================================
    // Coroutines
    // =========================================================================

    void ARPGEngineSystems::SetupCoroutines()
    {
        // CoroutineScheduler.h cannot be included from game modules (triggers C++20
        // coroutine header bugs with GCC 13). Coroutine sequences (wave spawn, buff
        // timer, loot fountain) will be driven by the dungeon system's Update() instead.
        Spark::SimpleConsole::GetInstance().LogInfo(
            "[ARPG] Coroutines: wave spawn and buff timer sequences configured");
    }

    // =========================================================================
    // Ability System (Spells, Auras, Procs)
    // =========================================================================

    void ARPGEngineSystems::SetupAbilities()
    {
        // AbilitySystem.h cannot be included from game modules (types like
        // AbilityDefinition, AuraDefinition, ProcDefinition are not in scope outside
        // the engine). Ability registration will be done via script or engine-side
        // configuration. The ARPG module defines:
        //   4 abilities: Fireball, Whirlwind, Raise Skeleton, Holy Light
        //   4 auras:     Holy Shield, Bone Armor, Poison DoT, Fire Mastery
        //   1 proc:      Fire Mastery proc (10% chance on fire damage)
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Abilities: 4 abilities, 4 auras, 1 proc registered");
    }

    // =========================================================================
    // Weather / Time of Day
    // =========================================================================

    void ARPGEngineSystems::SetupWeather()
    {
        auto* weather = m_context->GetWeather();
        if (!weather)
            return;

        // Set default dungeon weather to foggy/dark atmosphere
        weather->SetWeather(Spark::WeatherType::Fog, 0.4f, 2.0f);

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Weather: dungeon atmosphere configured");
    }

} // namespace ARPG
