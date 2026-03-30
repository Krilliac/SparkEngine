/**
 * @file RTSCommandSystem.cpp
 * @brief Unit selection, command issuing, and command queue processing
 */

#include "RTSCommandSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>

namespace RTS
{

    bool RTSCommandSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "RTS command system initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Command system initialized");
        return true;
    }

    void RTSCommandSystem::Update(float deltaTime)
    {
        ProcessCommands(deltaTime);
    }

    void RTSCommandSystem::Shutdown()
    {
        m_selectedUnits.clear();
        m_commandQueues.clear();
    }

    // === Selection ===

    void RTSCommandSystem::Select(uint32_t unitId)
    {
        m_selectedUnits.clear();
        m_selectedUnits.push_back(unitId);
    }

    void RTSCommandSystem::AddToSelection(uint32_t unitId)
    {
        // Prevent duplicates
        for (uint32_t id : m_selectedUnits)
        {
            if (id == unitId)
                return;
        }
        m_selectedUnits.push_back(unitId);
    }

    void RTSCommandSystem::DeselectAll()
    {
        m_selectedUnits.clear();
    }

    void RTSCommandSystem::SelectAllOfType(RTSUnitType type, RTSFaction faction)
    {
        // This would query the unit system for matching units
        // For now, keep existing selection logic as a placeholder
        (void)type;
        (void)faction;
    }

    const std::vector<uint32_t>& RTSCommandSystem::GetSelection() const
    {
        return m_selectedUnits;
    }

    size_t RTSCommandSystem::GetSelectionCount() const
    {
        return m_selectedUnits.size();
    }

    // === Commands ===

    void RTSCommandSystem::IssueCommand(uint32_t unitId, const UnitCommand& command)
    {
        // Replace existing queue with single command
        m_commandQueues[unitId].clear();
        m_commandQueues[unitId].push_back(command);
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "RTS command issued to unit %u (type=%d)", unitId,
                        static_cast<int>(command.type));
    }

    void RTSCommandSystem::QueueCommand(uint32_t unitId, const UnitCommand& command)
    {
        // Append to existing queue (shift-click behavior)
        m_commandQueues[unitId].push_back(command);
    }

    void RTSCommandSystem::ClearCommands(uint32_t unitId)
    {
        m_commandQueues.erase(unitId);
    }

    void RTSCommandSystem::IssueCommandToSelection(const UnitCommand& command)
    {
        for (uint32_t unitId : m_selectedUnits)
        {
            IssueCommand(unitId, command);
        }
    }

    // === Queries ===

    const UnitCommand* RTSCommandSystem::GetCurrentCommand(uint32_t unitId) const
    {
        auto it = m_commandQueues.find(unitId);
        if (it == m_commandQueues.end() || it->second.empty())
            return nullptr;

        return &it->second.front();
    }

    size_t RTSCommandSystem::GetPendingCommandCount() const
    {
        size_t total = 0;
        for (const auto& [id, queue] : m_commandQueues)
        {
            total += queue.size();
        }
        return total;
    }

    std::string RTSCommandSystem::GetCommandStatusString() const
    {
        std::string result = "=== RTS Commands ===\n";
        result += "Selected units: " + std::to_string(m_selectedUnits.size()) + "\n";
        result += "Units with commands: " + std::to_string(m_commandQueues.size()) + "\n";
        result += "Total pending: " + std::to_string(GetPendingCommandCount()) + "\n";
        return result;
    }

    // === Internal ===

    void RTSCommandSystem::ProcessCommands(float deltaTime)
    {
        (void)deltaTime;

        // Process command queues: pop completed commands, advance to next
        for (auto it = m_commandQueues.begin(); it != m_commandQueues.end();)
        {
            auto& queue = it->second;

            if (queue.empty())
            {
                it = m_commandQueues.erase(it);
                continue;
            }

            // The front command is the active one.
            // In a full implementation, this would:
            // 1. Move units toward targetX/targetY for Move commands
            // 2. Engage targets for Attack commands
            // 3. Toggle between two waypoints for Patrol
            // 4. Keep position for Hold
            // 5. Send workers to build sites for Build
            // 6. Send workers to resource nodes for Gather
            // 7. Immediately stop for Stop (clear queue)

            const auto& cmd = queue.front();
            if (cmd.type == RTSCommandType::Stop)
            {
                queue.clear();
            }

            ++it;
        }
    }

    void RTSCommandSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("RTS Command System"))
        {
            ImGui::Text("Selected units: %zu", m_selectedUnits.size());
            ImGui::Text("Command queues: %zu", m_commandQueues.size());
            ImGui::Text("Total pending: %zu", GetPendingCommandCount());

            if (ImGui::TreeNode("Selection"))
            {
                for (uint32_t id : m_selectedUnits)
                {
                    ImGui::Text("Unit %u", id);
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace RTS
