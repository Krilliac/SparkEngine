/**
 * @file InstanceManager.cpp
 * @brief Instance/dungeon management and encounter scripting implementation
 */

#include "InstanceManager.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cassert>

namespace Spark::Gameplay
{

    // ============================================================================
    // Encounter State Helpers
    // ============================================================================

    const char* EncounterStateToString(EncounterState state)
    {
        switch (state)
        {
        case EncounterState::NotStarted:
            return "NotStarted";
        case EncounterState::InProgress:
            return "InProgress";
        case EncounterState::Done:
            return "Done";
        case EncounterState::Failed:
            return "Failed";
        }
        return "Unknown";
    }

    // ============================================================================
    // InstanceData
    // ============================================================================

    bool InstanceData::AllEncountersDone() const
    {
        // An instance is complete when every non-optional encounter is Done.
        // We need the template to check the optional flag, but encounter states
        // are stored here. Since we only store non-optional encounters as required,
        // we check all states — optional encounters that are NotStarted are fine.
        for (const auto& [encId, state] : encounterStates)
        {
            if (state != EncounterState::Done)
            {
                // If we cannot tell whether it is optional from here, treat all as required.
                // The InstanceManager checks optional status at the CompleteEncounter level.
                return false;
            }
        }
        return true;
    }

    int InstanceData::GetCompletedEncounterCount() const
    {
        int count = 0;
        for (const auto& [encId, state] : encounterStates)
        {
            if (state == EncounterState::Done)
            {
                ++count;
            }
        }
        return count;
    }

    // ============================================================================
    // InstanceLockout
    // ============================================================================

    bool InstanceLockout::IsExpired() const
    {
        return std::chrono::system_clock::now() >= expiresAt;
    }

    // ============================================================================
    // InstanceManager — Singleton
    // ============================================================================

    InstanceManager& InstanceManager::GetInstance()
    {
        static InstanceManager instance;
        return instance;
    }

    void InstanceManager::Initialize()
    {
        m_nextId = 1;
        m_templates.clear();
        m_scriptFactories.clear();
        m_instances.clear();
        m_playerInstances.clear();
        m_lockouts.clear();
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InstanceManager initialized");
    }

    void InstanceManager::Update(float deltaTime)
    {
        for (auto& [instId, instData] : m_instances)
        {
            for (auto& [encId, state] : instData.encounterStates)
            {
                if (state != EncounterState::InProgress)
                {
                    continue;
                }

                // Update elapsed time on the script
                auto scriptIt = instData.encounterScripts.find(encId);
                if (scriptIt != instData.encounterScripts.end() && scriptIt->second)
                {
                    scriptIt->second->m_elapsed += deltaTime;
                    scriptIt->second->OnUpdate(deltaTime);
                }
            }
        }
    }

    void InstanceManager::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "InstanceManager shutting down (%zu instances, %zu templates)",
                       m_instances.size(), m_templates.size());
        m_instances.clear();
        m_playerInstances.clear();
        m_lockouts.clear();
        m_scriptFactories.clear();
        m_templates.clear();
    }

    // ============================================================================
    // Template Registration
    // ============================================================================

    void InstanceManager::RegisterTemplate(const InstanceTemplate& tmpl)
    {
        m_templates[tmpl.templateId] = tmpl;
    }

    void InstanceManager::RegisterEncounterScript(uint32_t templateId, EncounterID encounterId,
                                                  EncounterScriptFactory factory)
    {
        m_scriptFactories[templateId][encounterId] = std::move(factory);
    }

    const InstanceTemplate* InstanceManager::GetTemplate(uint32_t templateId) const
    {
        auto it = m_templates.find(templateId);
        if (it == m_templates.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    // ============================================================================
    // Instance Lifecycle
    // ============================================================================

    InstanceID InstanceManager::CreateInstance(uint32_t templateId)
    {
        auto tmplIt = m_templates.find(templateId);
        if (tmplIt == m_templates.end())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "CreateInstance failed — template %u not found", templateId);
            return 0; // Invalid — template not found
        }

        const auto& tmpl = tmplIt->second;
        InstanceID newId = m_nextId++;

        InstanceData data;
        data.id = newId;
        data.templateId = templateId;
        data.createdAt = std::chrono::system_clock::now();

        // Initialize encounter states and create scripts from registered factories
        for (const auto& encounter : tmpl.encounters)
        {
            data.encounterStates[encounter.id] = EncounterState::NotStarted;

            auto factoryMapIt = m_scriptFactories.find(templateId);
            if (factoryMapIt != m_scriptFactories.end())
            {
                auto factoryIt = factoryMapIt->second.find(encounter.id);
                if (factoryIt != factoryMapIt->second.end())
                {
                    data.encounterScripts[encounter.id] = factoryIt->second();
                }
            }
        }

        m_instances[newId] = std::move(data);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Created instance %u from template %u (%zu encounters)", newId,
                       templateId, tmpl.encounters.size());
        return newId;
    }

    void InstanceManager::DestroyInstance(InstanceID id)
    {
        auto it = m_instances.find(id);
        if (it == m_instances.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "DestroyInstance: instance %u not found", id);
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "Destroying instance %u (%zu players)", id, it->second.players.size());

        // Remove all player reverse-lookup entries for this instance
        for (EntityID player : it->second.players)
        {
            m_playerInstances.erase(player);
        }

        m_instances.erase(it);
    }

    InstanceData* InstanceManager::GetInstanceData(InstanceID id)
    {
        auto it = m_instances.find(id);
        if (it == m_instances.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    const InstanceData* InstanceManager::GetInstanceData(InstanceID id) const
    {
        auto it = m_instances.find(id);
        if (it == m_instances.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    // ============================================================================
    // Player Management
    // ============================================================================

    bool InstanceManager::AddPlayer(InstanceID id, EntityID player)
    {
        auto instIt = m_instances.find(id);
        if (instIt == m_instances.end())
        {
            return false;
        }

        auto& instData = instIt->second;

        // Check max players from the template
        auto tmplIt = m_templates.find(instData.templateId);
        if (tmplIt != m_templates.end())
        {
            int maxPlayers = tmplIt->second.maxPlayers;
            if (static_cast<int>(instData.players.size()) >= maxPlayers)
            {
                return false; // Instance full
            }
        }

        // Check lockout — player cannot re-enter a cleared instance
        if (HasLockout(player, instData.templateId))
        {
            return false;
        }

        // Prevent duplicate adds
        auto playerIt = std::find(instData.players.begin(), instData.players.end(), player);
        if (playerIt != instData.players.end())
        {
            return false; // Already in this instance
        }

        instData.players.push_back(player);
        m_playerInstances[player] = id;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Player %u added to instance %u (%zu/%d players)", player, id,
                       instData.players.size(), tmplIt != m_templates.end() ? tmplIt->second.maxPlayers : -1);
        return true;
    }

    void InstanceManager::RemovePlayer(InstanceID id, EntityID player)
    {
        auto instIt = m_instances.find(id);
        if (instIt == m_instances.end())
        {
            return;
        }

        auto& players = instIt->second.players;
        auto it = std::find(players.begin(), players.end(), player);
        if (it != players.end())
        {
            players.erase(it);
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Player %u removed from instance %u", player, id);
        }

        m_playerInstances.erase(player);
    }

    InstanceID InstanceManager::GetPlayerInstance(EntityID player) const
    {
        auto it = m_playerInstances.find(player);
        if (it == m_playerInstances.end())
        {
            return 0; // Not in any instance
        }
        return it->second;
    }

    // ============================================================================
    // Encounter Control
    // ============================================================================

    bool InstanceManager::StartEncounter(InstanceID instanceId, EncounterID encounterId)
    {
        auto* instData = GetInstanceData(instanceId);
        if (!instData)
        {
            return false;
        }

        auto stateIt = instData->encounterStates.find(encounterId);
        if (stateIt == instData->encounterStates.end())
        {
            return false; // Encounter not in this instance
        }

        // Can only start from NotStarted or Failed (retry after wipe)
        if (stateIt->second != EncounterState::NotStarted && stateIt->second != EncounterState::Failed)
        {
            return false;
        }

        stateIt->second = EncounterState::InProgress;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Encounter %u started in instance %u", encounterId, instanceId);

        auto scriptIt = instData->encounterScripts.find(encounterId);
        if (scriptIt != instData->encounterScripts.end() && scriptIt->second)
        {
            scriptIt->second->m_state = EncounterState::InProgress;
            scriptIt->second->m_elapsed = 0.0f;
            scriptIt->second->OnStart(instanceId, encounterId);
        }

        return true;
    }

    void InstanceManager::FailEncounter(InstanceID instanceId, EncounterID encounterId)
    {
        auto* instData = GetInstanceData(instanceId);
        if (!instData)
        {
            return;
        }

        auto stateIt = instData->encounterStates.find(encounterId);
        if (stateIt == instData->encounterStates.end() || stateIt->second != EncounterState::InProgress)
        {
            return;
        }

        stateIt->second = EncounterState::Failed;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Encounter %u failed in instance %u", encounterId, instanceId);

        auto scriptIt = instData->encounterScripts.find(encounterId);
        if (scriptIt != instData->encounterScripts.end() && scriptIt->second)
        {
            scriptIt->second->m_state = EncounterState::Failed;
            scriptIt->second->OnWipe();
        }
    }

    void InstanceManager::CompleteEncounter(InstanceID instanceId, EncounterID encounterId)
    {
        auto* instData = GetInstanceData(instanceId);
        if (!instData)
        {
            return;
        }

        auto stateIt = instData->encounterStates.find(encounterId);
        if (stateIt == instData->encounterStates.end() || stateIt->second != EncounterState::InProgress)
        {
            return;
        }

        stateIt->second = EncounterState::Done;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Encounter %u completed in instance %u", encounterId, instanceId);

        auto scriptIt = instData->encounterScripts.find(encounterId);
        if (scriptIt != instData->encounterScripts.end() && scriptIt->second)
        {
            scriptIt->second->m_state = EncounterState::Done;
            scriptIt->second->OnBossDeath(0); // Boss entity ID would come from caller
        }

        // Check if the entire instance is now complete. encounterStates carries EVERY
        // encounter (required and optional), so a plain "all states Done" test never
        // completes an instance whose optional encounters were skipped. Cross-reference
        // the template's optional flags: an encounter is satisfied if it is Done, or if
        // it is optional and not currently Failed. Iterate the template so optionality
        // is available.
        const InstanceTemplate* tmpl = GetTemplate(instData->templateId);
        bool instanceComplete = true;
        if (tmpl)
        {
            for (const auto& encounter : tmpl->encounters)
            {
                auto encStateIt = instData->encounterStates.find(encounter.id);
                EncounterState encState =
                    (encStateIt != instData->encounterStates.end()) ? encStateIt->second : EncounterState::NotStarted;

                if (encState == EncounterState::Done)
                {
                    continue;
                }
                if (encounter.optional && encState != EncounterState::Failed)
                {
                    continue; // Skipped/unstarted optional encounters do not block completion.
                }
                instanceComplete = false;
                break;
            }
        }
        else
        {
            // Template missing (should not happen for a live instance) — fall back to the
            // strict "every encounter Done" rule.
            instanceComplete = instData->AllEncountersDone();
        }

        if (instanceComplete)
        {
            instData->completed = true;
            SPARK_LOG_INFO(Spark::LogCategory::Core,
                           "Instance %u fully completed — all required encounters done", instanceId);
        }
    }

    void InstanceManager::ResetEncounter(InstanceID instanceId, EncounterID encounterId)
    {
        auto* instData = GetInstanceData(instanceId);
        if (!instData)
        {
            return;
        }

        auto stateIt = instData->encounterStates.find(encounterId);
        if (stateIt == instData->encounterStates.end())
        {
            return;
        }

        stateIt->second = EncounterState::NotStarted;

        auto scriptIt = instData->encounterScripts.find(encounterId);
        if (scriptIt != instData->encounterScripts.end() && scriptIt->second)
        {
            scriptIt->second->m_state = EncounterState::NotStarted;
            scriptIt->second->m_elapsed = 0.0f;
            scriptIt->second->m_currentPhase = 0;
            scriptIt->second->OnReset();
        }
    }

    EncounterState InstanceManager::GetEncounterState(InstanceID instanceId, EncounterID encounterId) const
    {
        const auto* instData = GetInstanceData(instanceId);
        if (!instData)
        {
            return EncounterState::NotStarted;
        }

        auto it = instData->encounterStates.find(encounterId);
        if (it == instData->encounterStates.end())
        {
            return EncounterState::NotStarted;
        }
        return it->second;
    }

    // ============================================================================
    // Lockout
    // ============================================================================

    bool InstanceManager::HasLockout(EntityID player, uint32_t templateId) const
    {
        for (const auto& lockout : m_lockouts)
        {
            if (lockout.playerId == player && lockout.templateId == templateId && !lockout.IsExpired())
            {
                return true;
            }
        }
        return false;
    }

    void InstanceManager::AddLockout(EntityID player, uint32_t templateId, float durationSeconds)
    {
        InstanceLockout lockout;
        lockout.playerId = player;
        lockout.templateId = templateId;
        lockout.expiresAt =
            std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                   std::chrono::duration<float>(durationSeconds));
        m_lockouts.push_back(std::move(lockout));
    }

    void InstanceManager::ClearExpiredLockouts()
    {
        std::erase_if(m_lockouts, [](const InstanceLockout& lockout) { return lockout.IsExpired(); });
    }

} // namespace Spark::Gameplay
