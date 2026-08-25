/**
 * @file ARPGEngineSystems.cpp
 * @brief Implementation of engine system integrations for the ARPG module
 */

#include "ARPGEngineSystems.h"
#include "ARPGAbilityCatalog.h"
#include "Hero/ARPGHeroSystem.h"
#include "Combat/ARPGCombatSystem.h"
#include "Loot/ARPGLootSystem.h"
#include "Dungeon/ARPGDungeonSystem.h"

#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/AI/AISystem.h"
#include "Engine/AI/BehaviorTree.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Gameplay/AbilitySystem.h"
#include "Graphics/WeatherSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace ARPG
{

    namespace
    {
        constexpr const char* IDLE_STATE = "Idle";
        constexpr const char* MOVE_STATE = "Run";
        constexpr const char* ATTACK_STATE = "Attack";
        constexpr const char* CAST_STATE = "Cast";
        constexpr const char* DEATH_STATE = "Die";

        Spark::Animation::AnimationState MakeAnimationState(const char* name, const char* clipName, bool loop)
        {
            Spark::Animation::AnimationState state;
            state.name = name;
            state.clipName = clipName;
            state.speed = 1.0f;
            state.loop = loop;
            return state;
        }

        std::shared_ptr<Spark::Animation::AnimationClip> MakeAnimationClip(const char* name, float duration, bool loop)
        {
            auto clip = std::make_shared<Spark::Animation::AnimationClip>();
            clip->name = name;
            clip->duration = duration;
            clip->ticksPerSecond = 30.0f;
            clip->loop = loop;
            return clip;
        }
    } // namespace

    // =========================================================================
    // Lifecycle
    // =========================================================================

    ARPGEngineSystems::ARPGEngineSystems() = default;

    ARPGEngineSystems::~ARPGEngineSystems()
    {
        Shutdown();
    }

    bool ARPGEngineSystems::Initialize(Spark::IEngineContext* context, ARPGHeroSystem* heroes, ARPGCombatSystem* combat,
                                       ARPGLootSystem* loot, ARPGDungeonSystem* dungeon)
    {
        if (!context || !heroes || !combat || !loot || !dungeon)
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
        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG engine systems integration initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Engine systems integration initialized");
        return true;
    }

    void ARPGEngineSystems::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        if (m_heroAnimation && std::isfinite(deltaTime) && deltaTime > 0.0f)
            m_heroAnimation->Update(deltaTime);

        if (!m_actionUsesCoroutine && m_actionTimeRemaining > 0.0f && std::isfinite(deltaTime) && deltaTime > 0.0f)
        {
            m_actionTimeRemaining = std::max(0.0f, m_actionTimeRemaining - deltaTime);
            if (m_actionTimeRemaining <= 0.0f)
                CompleteHeroAction(m_actionGeneration);
        }
    }

    void ARPGEngineSystems::Shutdown()
    {
        if (!m_initialized)
            return;

        // RAII handles auto-unsubscribe, but clear explicitly for deterministic order
        m_eventHandles.clear();

        ++m_actionGeneration;
        if (m_context && !m_actionCoroutineName.empty())
        {
            if (auto* scheduler = m_context->GetCoroutineScheduler())
                scheduler->StopCoroutine(m_actionCoroutineName);
        }

        m_heroAnimation.reset();
        m_actionCoroutineName.clear();
        m_actionTimeRemaining = 0.0f;
        m_actionUsesCoroutine = false;
        m_hasAnimationBridge = false;
        m_hasAbilityBridge = false;
        m_registeredAbilityCount = 0;
        m_registeredAuraCount = 0;
        m_registeredProcCount = 0;

        m_initialized = false;
        m_context = nullptr;
        m_heroes = nullptr;
        m_combat = nullptr;
        m_loot = nullptr;
        m_dungeon = nullptr;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG engine systems integration shut down");
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
            ImGui::Text("Hero animation: %s", GetHeroAnimationState().c_str());
            ImGui::Text("Action recovery: %s", m_actionUsesCoroutine ? "engine coroutine" : "local timer");
            ImGui::Text("Abilities: %u abilities, %u auras, %u proc registered", m_registeredAbilityCount,
                        m_registeredAuraCount, m_registeredProcCount);

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

        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG EventBus: 2 subscriptions registered");
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

        SPARK_LOG_INFO(Spark::LogCategory::Game, "ARPG AI: 3 behavior trees registered");
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] AI: 3 behavior trees registered");
    }

    // =========================================================================
    // Animation State Machines
    // =========================================================================

    void ARPGEngineSystems::SetupAnimation()
    {
        m_heroAnimation = std::make_unique<Spark::Animation::AnimationStateMachine>();

        constexpr const char* IdleClip = "arpg_hero_idle";
        constexpr const char* RunClip = "arpg_hero_run";
        constexpr const char* AttackClip = "arpg_hero_attack";
        constexpr const char* CastClip = "arpg_hero_cast";
        constexpr const char* DeathClip = "arpg_hero_death";

        m_heroAnimation->AddState(MakeAnimationState(IDLE_STATE, IdleClip, true));
        m_heroAnimation->AddState(MakeAnimationState(MOVE_STATE, RunClip, true));
        m_heroAnimation->AddState(MakeAnimationState(ATTACK_STATE, AttackClip, false));
        m_heroAnimation->AddState(MakeAnimationState(CAST_STATE, CastClip, false));
        m_heroAnimation->AddState(MakeAnimationState(DEATH_STATE, DeathClip, false));
        m_heroAnimation->SetDefaultState(IDLE_STATE);
        m_heroAnimation->ForceState(IDLE_STATE);

        if (auto* animation = m_context->GetAnimation())
        {
            animation->RegisterClip(IdleClip, MakeAnimationClip(IdleClip, 1.0f, true));
            animation->RegisterClip(RunClip, MakeAnimationClip(RunClip, 0.75f, true));
            animation->RegisterClip(
                AttackClip, MakeAnimationClip(AttackClip, GetActionDuration(ARPGHeroAction::BasicAttack), false));
            animation->RegisterClip(CastClip,
                                    MakeAnimationClip(CastClip, GetActionDuration(ARPGHeroAction::Cast), false));
            animation->RegisterClip(DeathClip, MakeAnimationClip(DeathClip, 1.2f, false));
            m_hasAnimationBridge = true;
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Animation: live hero state machine registered");
    }

    // =========================================================================
    // Coroutines
    // =========================================================================

    void ARPGEngineSystems::SetupCoroutines()
    {
        m_actionCoroutineName =
            "arpg.hero_action_recovery." + std::to_string(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)));
        const bool available = m_context->GetCoroutineScheduler() != nullptr;
        Spark::SimpleConsole::GetInstance().LogInfo(
            available ? "[ARPG] Coroutines: hero action recovery connected"
                      : "[ARPG] Coroutines: scheduler unavailable, using deterministic local recovery");
    }

    // =========================================================================
    // Ability System (Spells, Auras, Procs)
    // =========================================================================

    void ARPGEngineSystems::SetupAbilities()
    {
        auto* abilities = m_context->GetAbilities();
        if (!abilities)
        {
            Spark::SimpleConsole::GetInstance().LogWarning("[ARPG] Abilities: engine registry unavailable");
            return;
        }

        ARPGAbilityCatalog::Register(*abilities);

        m_hasAbilityBridge = true;
        m_registeredAbilityCount = 4;
        m_registeredAuraCount = 4;
        m_registeredProcCount = 1;
        Spark::SimpleConsole::GetInstance().LogInfo("[ARPG] Abilities: 4 abilities, 4 auras, 1 proc registered");
    }

    void ARPGEngineSystems::PlayHeroAction(ARPGHeroAction action)
    {
        if (!m_initialized || !m_heroAnimation)
            return;

        const char* state = IDLE_STATE;
        switch (action)
        {
        case ARPGHeroAction::Idle:
            state = IDLE_STATE;
            break;
        case ARPGHeroAction::Move:
            state = MOVE_STATE;
            break;
        case ARPGHeroAction::BasicAttack:
            state = ATTACK_STATE;
            break;
        case ARPGHeroAction::Cast:
            state = CAST_STATE;
            break;
        case ARPGHeroAction::Death:
            state = DEATH_STATE;
            break;
        }

        ++m_actionGeneration;
        m_heroAnimation->ForceState(state);
        m_actionTimeRemaining = GetActionDuration(action);
        m_actionUsesCoroutine = false;

        if (m_context && !m_actionCoroutineName.empty())
        {
            if (auto* scheduler = m_context->GetCoroutineScheduler())
            {
                scheduler->StopCoroutine(m_actionCoroutineName);
                if (m_actionTimeRemaining > 0.0f)
                {
                    const uint64_t generation = m_actionGeneration;
                    scheduler->StartCoroutine(m_actionCoroutineName)
                        .WaitForSeconds(m_actionTimeRemaining)
                        .Do([this, generation]() { CompleteHeroAction(generation); });
                    m_actionUsesCoroutine = true;
                }
            }
        }
    }

    std::string ARPGEngineSystems::GetHeroAnimationState() const
    {
        return m_heroAnimation ? m_heroAnimation->GetCurrentStateName() : std::string{};
    }

    bool ARPGEngineSystems::IsHeroActionActive() const
    {
        const std::string state = GetHeroAnimationState();
        return state == ATTACK_STATE || state == CAST_STATE;
    }

    void ARPGEngineSystems::CompleteHeroAction(uint64_t generation)
    {
        if (!m_initialized || generation != m_actionGeneration || !m_heroAnimation)
            return;

        m_heroAnimation->ForceState(IDLE_STATE);
        m_actionTimeRemaining = 0.0f;
        m_actionUsesCoroutine = false;
    }

    float ARPGEngineSystems::GetActionDuration(ARPGHeroAction action)
    {
        switch (action)
        {
        case ARPGHeroAction::BasicAttack:
            return 0.45f;
        case ARPGHeroAction::Cast:
            return 0.65f;
        default:
            return 0.0f;
        }
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
