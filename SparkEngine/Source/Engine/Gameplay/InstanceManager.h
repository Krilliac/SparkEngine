/**
 * @file InstanceManager.h
 * @brief Instance/dungeon management with encounter scripting (TC-inspired)
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages instanced content: dungeons, raids, arenas, scenarios.
 * Each instance has an EncounterScript controlling boss encounters.
 *
 * TrinityCore patterns:
 * - InstanceScript -> EncounterScript (per-instance lifecycle)
 * - Encounter state machine: NotStarted -> InProgress -> Done/Failed
 * - Instance lockout/reset tracking
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Gameplay
{

    using InstanceID = uint32_t;
    using EncounterID = uint32_t;
    using EntityID = uint32_t;

    // ============================================================================
    // Encounter State Machine
    // ============================================================================

    enum class EncounterState : uint8_t
    {
        NotStarted,
        InProgress,
        Done,
        Failed
    };

    /**
     * @brief Convert encounter state to a human-readable string
     * @param state The encounter state
     * @return Null-terminated string representation
     */
    const char* EncounterStateToString(EncounterState state);

    /**
     * @brief Defines a single encounter (boss fight) within an instance template
     */
    struct EncounterDefinition
    {
        EncounterID id = 0;              ///< Unique encounter identifier.
        std::string name;                ///< Boss/encounter name (e.g. "Lich King").
        std::string description;         ///< Encounter description for the journal UI.
        uint32_t bossEntityTemplate = 0; ///< Template ID for the boss entity to spawn.
        int requiredPlayers = 1;         ///< Minimum players needed to start the encounter.
        float enrageTimer = 0.0f;        ///< Seconds before soft enrage (0 = no enrage).
        bool optional = false;           ///< If false, must complete to finish the instance.
    };

    // ============================================================================
    // Encounter Script -- per-encounter behavior
    // ============================================================================

    /**
     * @brief Base class for encounter (boss fight) scripts
     *
     * Override to implement boss phases, add mechanics, spawn adds, etc.
     * Similar to TC's BossAI / InstanceScript encounter handling.
     */
    class EncounterScript
    {
      public:
        virtual ~EncounterScript() = default;

        virtual void OnStart(InstanceID instanceId, EncounterID encounterId) {}
        virtual void OnUpdate(float deltaTime) {}
        virtual void OnPlayerDeath(EntityID player) {}
        virtual void OnBossDeath(EntityID boss) {}
        virtual void OnWipe() {}
        virtual void OnReset() {}
        virtual void OnPhaseChange(int newPhase) {}

        EncounterState GetState() const { return m_state; }
        int GetCurrentPhase() const { return m_currentPhase; }
        float GetElapsedTime() const { return m_elapsed; }

      protected:
        void SetState(EncounterState state) { m_state = state; }

        void SetPhase(int phase)
        {
            m_currentPhase = phase;
            OnPhaseChange(phase);
        }

      private:
        EncounterState m_state = EncounterState::NotStarted;
        int m_currentPhase = 0;
        float m_elapsed = 0.0f;

        friend class InstanceManager; // InstanceManager updates elapsed
    };

    using EncounterScriptFactory = std::function<std::unique_ptr<EncounterScript>()>;

    // ============================================================================
    // Instance Template & Instance
    // ============================================================================

    /**
     * @brief Blueprint for an instance type (dungeon, raid, arena, etc.)
     */
    struct InstanceTemplate
    {
        uint32_t templateId = 0;
        std::string name;
        std::string description;
        int maxPlayers = 5;
        float resetTimerSeconds = 86400.0f; ///< 24 hours default
        std::vector<EncounterDefinition> encounters;
    };

    /**
     * @brief Runtime state for a live instance
     */
    struct InstanceData
    {
        InstanceID id = 0;
        uint32_t templateId = 0;
        std::vector<EntityID> players;
        std::unordered_map<EncounterID, EncounterState> encounterStates;
        std::unordered_map<EncounterID, std::unique_ptr<EncounterScript>> encounterScripts;
        std::chrono::system_clock::time_point createdAt;
        bool completed = false;

        /**
         * @brief Check if all non-optional encounters are Done
         */
        bool AllEncountersDone() const;

        /**
         * @brief Count how many encounters have state Done
         */
        int GetCompletedEncounterCount() const;
    };

    /**
     * @brief Lockout tracking -- prevents re-running cleared instances
     */
    struct InstanceLockout
    {
        uint32_t templateId = 0;
        EntityID playerId = 0;
        std::chrono::system_clock::time_point expiresAt;
        std::unordered_map<EncounterID, bool> completedEncounters;

        /**
         * @brief Check if this lockout has expired
         */
        bool IsExpired() const;
    };

    // ============================================================================
    // Instance Manager
    // ============================================================================

    /**
     * @brief Manages creation, lifecycle, and cleanup of instanced content
     *
     * Singleton that owns all live instances, tracks player-to-instance mappings,
     * and drives encounter script updates each frame.
     */
    class InstanceManager
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<InstanceManager>() instead")]]
        static InstanceManager& GetInstance();

        void Initialize();
        void Update(float deltaTime);
        void Shutdown();

        // -- Template Registration --

        void RegisterTemplate(const InstanceTemplate& tmpl);
        void RegisterEncounterScript(uint32_t templateId, EncounterID encounterId, EncounterScriptFactory factory);
        const InstanceTemplate* GetTemplate(uint32_t templateId) const;

        // -- Instance Lifecycle --

        InstanceID CreateInstance(uint32_t templateId);
        void DestroyInstance(InstanceID id);
        InstanceData* GetInstanceData(InstanceID id);
        const InstanceData* GetInstanceData(InstanceID id) const;

        // -- Player Management --

        bool AddPlayer(InstanceID id, EntityID player);
        void RemovePlayer(InstanceID id, EntityID player);
        InstanceID GetPlayerInstance(EntityID player) const;

        // -- Encounter Control --

        bool StartEncounter(InstanceID instanceId, EncounterID encounterId);
        void FailEncounter(InstanceID instanceId, EncounterID encounterId);
        void CompleteEncounter(InstanceID instanceId, EncounterID encounterId);
        void ResetEncounter(InstanceID instanceId, EncounterID encounterId);
        EncounterState GetEncounterState(InstanceID instanceId, EncounterID encounterId) const;

        // -- Lockout --

        bool HasLockout(EntityID player, uint32_t templateId) const;
        void AddLockout(EntityID player, uint32_t templateId, float durationSeconds);
        void ClearExpiredLockouts();

        // -- Stats --

        int GetActiveInstanceCount() const { return static_cast<int>(m_instances.size()); }
        int GetRegisteredTemplateCount() const { return static_cast<int>(m_templates.size()); }

      private:
        InstanceManager() = default;

        InstanceID m_nextId = 1;
        std::unordered_map<uint32_t, InstanceTemplate> m_templates;
        std::unordered_map<uint32_t, std::unordered_map<EncounterID, EncounterScriptFactory>> m_scriptFactories;
        std::unordered_map<InstanceID, InstanceData> m_instances;
        std::unordered_map<EntityID, InstanceID> m_playerInstances; ///< Reverse lookup
        std::vector<InstanceLockout> m_lockouts;
    };

} // namespace Spark::Gameplay
