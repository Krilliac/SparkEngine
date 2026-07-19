/**
 * @file InstanceEncounters.cpp
 * @brief Encounter control and lockout management for InstanceManager
 *
 * Split from InstanceManager.cpp. Contains: StartEncounter, FailEncounter,
 * CompleteEncounter, ResetEncounter, GetEncounterState, HasLockout, AddLockout,
 * and ClearExpiredLockouts.
 */

#include "InstanceManager.h"
#include "../../Utils/Validate.h"

#include <chrono>
#include <vector>

namespace Spark::Gameplay
{

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
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Instance %u fully completed — all required encounters done",
                           instanceId);
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
