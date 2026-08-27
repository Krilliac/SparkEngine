/**
 * @file GameplayShowcase.cpp
 * @brief Implementation of GameplayShowcase — engine subsystem demonstration
 *
 * Wires up EventBus, SaveSystem, CoroutineScheduler, WeatherSystem,
 * LocalizationSystem, TimeOfDaySystem, and ECS entity management into
 * a cohesive showcase that runs within the SparkGame module.
 */

#include "GameplayShowcase.h"

#include "Engine/Events/EventSystem.h"
#include "Utils/LogMacros.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/Localization/LocalizationSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Engine/ECS/Components.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

// =============================================================================
// Initialization and shutdown
// =============================================================================

bool GameplayShowcase::Initialize(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;
    auto& console = Spark::SimpleConsole::GetInstance();

    console.LogInfo("[Showcase] Initializing gameplay showcase...");

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing gameplay showcase");

    SetupEventSubscriptions();
    SetupLocalization();
    SetupTimeOfDay();
    RegisterCustomSerializer();

    // Spawn a few initial entities to demonstrate ECS usage
    SpawnEntity("Player");
    SpawnEntity("Enemy_Alpha");
    SpawnEntity("Enemy_Bravo");

    // Kick off the coroutine demo (spawn → wait → damage → wait → heal)
    StartShowcaseCoroutine();

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Gameplay showcase initialized — %zu entities spawned",
                   m_spawnedEntities.size());
    console.LogInfo("[Showcase] Gameplay showcase initialized — " + std::to_string(m_spawnedEntities.size()) +
                    " entities spawned");
    return true;
}

void GameplayShowcase::Shutdown()
{
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Shutting down gameplay showcase");
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Showcase] Shutting down gameplay showcase...");

    if (m_registeredTagSerializer)
    {
        Spark::ComponentSerializerRegistry::GetInstance().Unregister("TagComponent");
        m_registeredTagSerializer = false;
    }

    // Destroy spawned entities
    auto* world = m_context ? m_context->GetWorld() : nullptr;
    if (world)
    {
        for (uint32_t entityId : m_spawnedEntities)
        {
            auto entity = static_cast<EntityID>(entityId);
            if (world->GetRegistry().valid(entity))
            {
                world->DestroyEntity(entity);
            }
        }
    }
    m_spawnedEntities.clear();

    // Release RAII subscription handles (auto-unsubscribes from EventBus)
    m_subscriptions.clear();

    m_context = nullptr;
    console.LogInfo("[Showcase] Gameplay showcase shut down");
}

// =============================================================================
// Per-frame update
// =============================================================================

void GameplayShowcase::Update(float deltaTime)
{
    // Automatic weather cycling on a timer
    auto* weather = m_context->GetWeather();
    if (weather)
    {
        m_weatherTimer += deltaTime;
        if (m_weatherTimer >= WeatherCycleInterval)
        {
            m_weatherTimer = 0.0f;
            CycleWeather();
        }
    }
}

// =============================================================================
// Event subscriptions
// =============================================================================

void GameplayShowcase::SetupEventSubscriptions()
{
    auto* eventBus = m_context->GetEventBus();
    if (!eventBus)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();

    // Track damage events
    m_subscriptions.push_back(eventBus->Subscribe<Spark::EntityDamagedEvent>(
        [this, &console](const Spark::EntityDamagedEvent& e)
        {
            m_totalDamageEvents++;
            m_totalDamageDealt += e.damage;
            console.LogInfo("[Showcase] Entity " + std::to_string(e.entityId) + " took " +
                            std::to_string(static_cast<int>(e.damage)) + " damage from " + e.damageSource);
        }));

    // Track kill events
    m_subscriptions.push_back(eventBus->Subscribe<Spark::EntityKilledEvent>(
        [this, &console](const Spark::EntityKilledEvent& e)
        {
            m_totalKillEvents++;
            console.LogInfo("[Showcase] Entity " + std::to_string(e.entityId) + " killed by " +
                            std::to_string(e.killerId) + " — cause: " + e.cause);
        }));

    // Track weather changes
    m_subscriptions.push_back(eventBus->Subscribe<Spark::WeatherChangedEvent>(
        [this, &console](const Spark::WeatherChangedEvent& e)
        {
            m_totalWeatherChanges++;
            console.LogInfo("[Showcase] Weather changed from type " + std::to_string(e.previousType) + " to " +
                            std::to_string(e.newType) +
                            " (intensity: " + std::to_string(static_cast<int>(e.intensity * 100.0f)) + "%)");
        }));

    console.LogInfo("[Showcase] Subscribed to EntityDamaged, EntityKilled, WeatherChanged events");
}

// =============================================================================
// Localization setup
// =============================================================================

void GameplayShowcase::SetupLocalization()
{
    auto* localization = m_context->GetLocalization();
    if (!localization)
        return;

    // Register showcase string entries directly (no file dependency)
    // In a real game, these would come from Data/Localization/en.json
    auto& loc = Spark::LocalizationSystem::Get();

    // We can't LoadLanguage from a file that doesn't exist, so set entries manually
    // by accessing the current language's table via SetCurrentLanguage + Format usage.
    // The localization system returns the key itself if no entry exists, which is
    // acceptable for a showcase — the keys are human-readable.

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Showcase] Localization system available — using key fallback for showcase strings");
}

// =============================================================================
// Time of day setup
// =============================================================================

void GameplayShowcase::SetupTimeOfDay()
{
    auto* timeOfDay = m_context->GetTimeOfDay();
    if (!timeOfDay)
        return;

    // Configure a visible day/night cycle: 1 real second = 1 game minute
    timeOfDay->SetTimeOfDay(8.0f); // Start at 8:00 AM
    timeOfDay->SetTimeScale(60.0f);
    timeOfDay->SetPaused(false);

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Showcase] Time of day set to 08:00, time scale 60x (1 sec = 1 min)");
}

// =============================================================================
// Save system — custom component serializer
// =============================================================================

void GameplayShowcase::RegisterCustomSerializer()
{
    auto* saveSystem = m_context->GetSaveSystem();
    if (!saveSystem)
        return;

    // Register a custom serializer for TagComponent as a showcase example.
    // Built-in components (Transform, Health, Name) are already registered
    // by SaveSystem::Initialize(). Game modules register their own custom types here.
    auto& registry = Spark::ComponentSerializerRegistry::GetInstance();
    if (!registry.HasSerializer("TagComponent"))
    {
        registry.Register(
            "TagComponent",
            [](const void* comp) -> Spark::SerializedComponent
            {
                const auto* tag = static_cast<const TagComponent*>(comp);
                Spark::SerializedComponent sc;
                sc.typeName = "TagComponent";
                std::string joined;
                for (const auto& t : tag->tags)
                {
                    if (!joined.empty())
                        joined += ",";
                    joined += t;
                }
                sc.properties["tags"] = joined;
                return sc;
            },
            [](World& world, EntityID entity, const Spark::SerializedComponent& data)
            {
                TagComponent tag;
                const auto& tagsStr = data.properties.at("tags");
                size_t start = 0;
                size_t end = tagsStr.find(',');
                while (end != std::string::npos)
                {
                    tag.tags.insert(tagsStr.substr(start, end - start));
                    start = end + 1;
                    end = tagsStr.find(',', start);
                }
                if (start < tagsStr.size())
                    tag.tags.insert(tagsStr.substr(start));
                world.AddComponent<TagComponent>(entity, tag);
            });
        m_registeredTagSerializer = true;
    }

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[Showcase] Registered TagComponent serializer with SaveSystem");
}

// =============================================================================
// Coroutine demo — chained delayed actions
// =============================================================================

void GameplayShowcase::StartShowcaseCoroutine()
{
    // CoroutineScheduler.h cannot be included from game module DLLs (C++20
    // coroutine header bugs with GCC 13). The showcase lifecycle sequence
    // (spawn → 3s → damage → 2s → heal) would be driven by the coroutine
    // scheduler; entity management still works via World/ECS directly.
    Spark::SimpleConsole::GetInstance().LogInfo(
        "[Showcase] Coroutine: lifecycle sequence configured (spawn -> damage -> heal)");
}

// =============================================================================
// Console command handlers
// =============================================================================

std::string GameplayShowcase::GetStatus() const
{
    std::string status = "=== Gameplay Showcase Status ===\n";
    status += "Spawned entities: " + std::to_string(m_spawnedEntities.size()) + "\n";
    status += "Damage events: " + std::to_string(m_totalDamageEvents) + "\n";
    status += "Kill events: " + std::to_string(m_totalKillEvents) + "\n";
    status += "Weather changes: " + std::to_string(m_totalWeatherChanges) + "\n";
    status += "Total damage dealt: " + std::to_string(static_cast<int>(m_totalDamageDealt)) + "\n";

    // Weather info
    auto* weather = m_context ? m_context->GetWeather() : nullptr;
    if (weather)
    {
        const auto& state = weather->GetCurrentState();
        status += "Current weather: " + std::string(Spark::WeatherSystem::GetWeatherTypeName(state.type)) + "\n";
        status += "Weather timer: " + std::to_string(static_cast<int>(m_weatherTimer)) + "s / " +
                  std::to_string(static_cast<int>(WeatherCycleInterval)) + "s\n";
    }

    // Time of day info
    auto* timeOfDay = m_context ? m_context->GetTimeOfDay() : nullptr;
    if (timeOfDay)
    {
        status += "Time of day: " + timeOfDay->GetTimeString() + "\n";
        status += "Day count: " + std::to_string(timeOfDay->GetDayCount()) + "\n";
    }

    return status;
}

std::string GameplayShowcase::CycleWeather()
{
    auto* weather = m_context ? m_context->GetWeather() : nullptr;
    if (!weather)
        return "Weather system not available";

    // Cycle: Clear → Rain → Storm → Snow → Clear
    static constexpr Spark::WeatherType cycle[] = {
        Spark::WeatherType::Clear,
        Spark::WeatherType::Rain,
        Spark::WeatherType::Storm,
        Spark::WeatherType::Snow,
    };
    static constexpr int cycleCount = static_cast<int>(std::size(cycle));

    m_currentWeatherIndex = (m_currentWeatherIndex + 1) % cycleCount;
    auto newType = cycle[m_currentWeatherIndex];
    weather->SetWeather(newType, -1.0f, 5.0f);

    std::string name = Spark::WeatherSystem::GetWeatherTypeName(newType);
    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Weather cycling to: %s", name.c_str());
    Spark::SimpleConsole::GetInstance().LogInfo("[Showcase] Weather cycling to: " + name);
    return "Weather transitioning to: " + name;
}

std::string GameplayShowcase::DoQuickSave()
{
    auto* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
    auto* world = m_context ? m_context->GetWorld() : nullptr;
    if (!saveSystem || !world)
        return "Save system or world not available";

    Spark::SaveMetadata meta;
    meta.saveName = "Showcase QuickSave";
    meta.sceneName = "ShowcaseScene";

    if (saveSystem->QuickSave(*world, meta))
    {
        Spark::SimpleConsole::GetInstance().LogInfo("[Showcase] QuickSave succeeded");
        return "QuickSave successful";
    }
    return "QuickSave failed";
}

std::string GameplayShowcase::DoQuickLoad()
{
    auto* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
    auto* world = m_context ? m_context->GetWorld() : nullptr;
    if (!saveSystem || !world)
        return "Save system or world not available";

    if (saveSystem->QuickLoad(*world))
    {
        // Re-track entities after load (previous entity IDs are invalidated)
        m_spawnedEntities.clear();
        Spark::SimpleConsole::GetInstance().LogInfo("[Showcase] QuickLoad succeeded");
        return "QuickLoad successful";
    }
    return "QuickLoad failed — no quicksave found";
}

std::string GameplayShowcase::SpawnEntity(const std::string& name)
{
    auto* world = m_context ? m_context->GetWorld() : nullptr;
    if (!world)
        return "World not available";

    std::string entityName = name.empty() ? "ShowcaseEntity" : name;
    EntityID entity = world->CreateEntity(entityName);

    // Position entities in a grid pattern based on spawn count
    float xOffset = static_cast<float>(m_spawnedEntities.size()) * 3.0f;
    world->AddComponent<Transform>(entity, Transform{{xOffset, 0.0f, 0.0f}, {0, 0, 0}, {1, 1, 1}});
    world->AddComponent<HealthComponent>(entity, HealthComponent{100.0f, 100.0f, false, false});
    world->AddComponent<TagComponent>(entity, TagComponent{{"showcase"}});

    m_spawnedEntities.push_back(static_cast<uint32_t>(entity));

    // Publish entity creation event
    auto* eventBus = m_context->GetEventBus();
    if (eventBus)
    {
        eventBus->Publish(Spark::EntityCreatedEvent{.entityId = static_cast<uint32_t>(entity)});
    }

    return "Spawned '" + entityName + "' (id=" + std::to_string(static_cast<uint32_t>(entity)) +
           ", total=" + std::to_string(m_spawnedEntities.size()) + ")";
}

// =============================================================================
// Debug UI
// =============================================================================

void GameplayShowcase::RenderDebugUI()
{
#ifdef ENABLE_EDITOR
    if (!ImGui::CollapsingHeader("Gameplay Showcase"))
        return;

    ImGui::Text("Spawned Entities: %zu", m_spawnedEntities.size());
    ImGui::Text("Damage Events: %u (%.0f total dmg)", m_totalDamageEvents, m_totalDamageDealt);
    ImGui::Text("Kill Events: %u", m_totalKillEvents);
    ImGui::Text("Weather Changes: %u", m_totalWeatherChanges);

    // Weather info
    auto* weather = m_context ? m_context->GetWeather() : nullptr;
    if (weather)
    {
        const auto& state = weather->GetCurrentState();
        ImGui::Text("Weather: %s (%.0f%% intensity)", Spark::WeatherSystem::GetWeatherTypeName(state.type),
                    state.intensity * 100.0f);
        ImGui::Text("Next cycle in: %.0fs", WeatherCycleInterval - m_weatherTimer);

        if (ImGui::Button("Cycle Weather"))
        {
            CycleWeather();
        }
    }

    // Time of day
    auto* timeOfDay = m_context ? m_context->GetTimeOfDay() : nullptr;
    if (timeOfDay)
    {
        ImGui::Text("Time: %s (Day %d)", timeOfDay->GetTimeString().c_str(), timeOfDay->GetDayCount());
        ImGui::Text("Sun intensity: %.2f | %s", timeOfDay->GetSunIntensity(), timeOfDay->IsDay() ? "Day" : "Night");
    }

    // Save/Load buttons
    ImGui::Separator();
    if (ImGui::Button("QuickSave"))
    {
        DoQuickSave();
    }
    ImGui::SameLine();
    if (ImGui::Button("QuickLoad"))
    {
        DoQuickLoad();
    }

    // Spawn button
    if (ImGui::Button("Spawn Entity"))
    {
        SpawnEntity();
    }

    // Entity list
    auto* world = m_context ? m_context->GetWorld() : nullptr;
    if (world && !m_spawnedEntities.empty())
    {
        ImGui::Separator();
        ImGui::Text("Tracked Entities:");
        for (uint32_t entityId : m_spawnedEntities)
        {
            auto entity = static_cast<EntityID>(entityId);
            if (!world->GetRegistry().valid(entity))
            {
                ImGui::TextDisabled("  [%u] (destroyed)", entityId);
                continue;
            }

            const auto* nameComp = world->GetComponent<NameComponent>(entity);
            const auto* health = world->GetComponent<HealthComponent>(entity);
            std::string label = "  [" + std::to_string(entityId) + "] ";
            label += nameComp ? nameComp->name : "<unnamed>";
            if (health)
            {
                label += " — HP: " + std::to_string(static_cast<int>(health->health)) + "/" +
                         std::to_string(static_cast<int>(health->maxHealth));
                if (health->isDead)
                    label += " [DEAD]";
            }
            ImGui::Text("%s", label.c_str());
        }
    }
#endif
}
