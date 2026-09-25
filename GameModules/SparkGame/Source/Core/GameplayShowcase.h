/**
 * @file GameplayShowcase.h
 * @brief Demonstrates usage of multiple SparkEngine subsystems from a game module
 * @author Spark Engine Team
 * @date 2026
 *
 * GameplayShowcase exercises EventBus, SaveSystem, CoroutineScheduler,
 * WeatherSystem, LocalizationSystem, TimeOfDaySystem, and ECS entity
 * lifecycle in a single cohesive class. Each subsystem demo is lightweight
 * and self-contained.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct HealthComponent;

/**
 * @brief Showcases core engine subsystem integration from a game module
 *
 * Demonstrates:
 * - EventBus subscription and publishing (damage, kill, weather events)
 * - SaveSystem quicksave/quickload with custom component serialization
 * - CoroutineScheduler chained delayed actions through IEngineContext::GetCoroutineScheduler():
 *   spawn a target, wait 3 s, deal 25 damage (EntityDamagedEvent), wait 2 s, heal it back
 * - WeatherSystem cycling through weather types on a timer
 * - LocalizationSystem string table loading and formatted lookups
 * - TimeOfDaySystem day/night cycle configuration
 * - ECS entity creation with NameComponent, Transform, HealthComponent
 * - MeshRenderer placement of the Blender-authored Engine Showcase kit (Assets/Models/Showcase/Kit)
 */
class GameplayShowcase
{
  public:
    /**
     * @brief Initialize all showcase subsystems
     * @param context Engine context for accessing subsystems
     * @return true if initialization succeeded
     */
    bool Initialize(Spark::IEngineContext* context);

    /** @brief Clean up subscriptions and spawned entities */
    void Shutdown();

    /**
     * @brief Per-frame update — ticks weather cycle timer and coroutines
     * @param deltaTime Frame delta in seconds
     */
    void Update(float deltaTime);

    /** @brief Render debug UI overlay (requires ENABLE_EDITOR / ImGui) */
    void RenderDebugUI();

    // --- Console command handlers ---

    /** @brief Return a status summary of all showcase subsystems */
    std::string GetStatus() const;

    /** @brief Cycle to the next weather type */
    std::string CycleWeather();

    /** @brief Quicksave the current ECS world state */
    std::string DoQuickSave();

    /** @brief Quickload the last saved world state */
    std::string DoQuickLoad();

    /**
     * @brief Spawn a named showcase entity with Transform and Health
     * @param name Entity name (defaults to "ShowcaseEntity" if empty)
     * @return Status string describing the spawned entity
     */
    std::string SpawnEntity(const std::string& name = "");

    /** @brief Scheduler name of the showcase lifecycle coroutine (stopped by name in Shutdown). */
    static constexpr const char* LifecycleCoroutineName = "SparkGame.ShowcaseLifecycle";
    /** @brief Seconds between the coroutine spawning its target and damaging it. */
    static constexpr float CoroutineDamageDelaySeconds = 3.0f;
    /** @brief Seconds between the coroutine damaging its target and healing it. */
    static constexpr float CoroutineHealDelaySeconds = 2.0f;
    /** @brief Health removed by the damage step and restored by the heal step. */
    static constexpr float CoroutineHealthDelta = 25.0f;

  private:
    void SetupEventSubscriptions();
    void SetupLocalization();
    void SetupTimeOfDay();
    void RegisterCustomSerializer();
    void StartShowcaseCoroutine();
    void SpawnExhibit();
    void SpawnCoroutineTarget();
    void DamageCoroutineTarget();
    void HealCoroutineTarget();
    void AbortCoroutineSequence(const std::string& reason);
    HealthComponent* FindCoroutineTargetHealth();

    Spark::IEngineContext* m_context{nullptr};

    // Event subscriptions (RAII — auto-unsubscribe on destruction)
    std::vector<Spark::SubscriptionHandle> m_subscriptions;

    // Tracked showcase entities for cleanup
    std::vector<uint32_t> m_spawnedEntities;

    // Exhibit prop entities (MeshRenderer only), kept apart so they do not shift the SpawnEntity grid
    std::vector<uint32_t> m_exhibitEntities;

    // True only when this module installed the TagComponent serializer. The
    // engine may already own a built-in registration, which must survive this
    // module's teardown.
    bool m_registeredTagSerializer{false};

    // Lifecycle coroutine state: the entity it drives and the step it last reached
    // (reported by GetStatus so the sequence is observable from the console).
    std::optional<uint32_t> m_coroutineTarget;
    std::string m_coroutineStage{"not started"};
    bool m_coroutineScheduled{false};

    // Weather cycling state
    int m_currentWeatherIndex{0};
    float m_weatherTimer{0.0f};
    static constexpr float WeatherCycleInterval = 30.0f;

    // Statistics tracked via events
    uint32_t m_totalDamageEvents{0};
    uint32_t m_totalKillEvents{0};
    uint32_t m_totalWeatherChanges{0};
    float m_totalDamageDealt{0.0f};
};
