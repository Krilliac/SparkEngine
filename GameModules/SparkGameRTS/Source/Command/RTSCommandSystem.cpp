/**
 * @file RTSCommandSystem.cpp
 * @brief Unit selection, command issuing, and command queue processing
 */

#include "RTSCommandSystem.h"
#include "Unit/RTSUnitSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace RTS
{

    bool RTSCommandSystem::Initialize(Spark::IEngineContext* context, RTSUnitSystem* unitSystem,
                                      const RTSBuildingSystem* buildingSystem)
    {
        if (!unitSystem)
            return false;

        m_context = context;
        m_unitSystem = unitSystem;
        m_buildingSystem = buildingSystem;
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
        m_unitSystem = nullptr;
        m_buildingSystem = nullptr;
        m_context = nullptr;
    }

    // === Selection ===

    void RTSCommandSystem::Select(uint32_t unitId)
    {
        m_selectedUnits.clear();
        if (m_unitSystem && m_unitSystem->GetUnit(unitId))
            m_selectedUnits.push_back(unitId);
    }

    void RTSCommandSystem::AddToSelection(uint32_t unitId)
    {
        if (!m_unitSystem || !m_unitSystem->GetUnit(unitId))
            return;

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
        m_selectedUnits.clear();
        if (!m_unitSystem)
            return;

        for (uint32_t unitId : m_unitSystem->GetUnitsByFaction(faction))
        {
            const UnitData* unit = m_unitSystem->GetUnit(unitId);
            if (unit && unit->type == type && unit->state != RTSUnitState::Dead)
                m_selectedUnits.push_back(unitId);
        }
        std::ranges::sort(m_selectedUnits);
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
        if (!m_unitSystem || !m_unitSystem->GetUnit(unitId) || !IsCommandValid(command))
            return;

        // Replace existing queue with single command; its route is planned by the simulation, never supplied.
        std::vector<UnitCommand>& queue = m_commandQueues[unitId];
        queue.clear();
        queue.push_back(command);
        queue.back().path.clear();
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "RTS command issued to unit %u (type=%d)", unitId,
                        static_cast<int>(command.type));
    }

    void RTSCommandSystem::QueueCommand(uint32_t unitId, const UnitCommand& command)
    {
        if (!m_unitSystem || !m_unitSystem->GetUnit(unitId) || !IsCommandValid(command))
            return;

        // Append to existing queue (shift-click behavior); a full queue ignores further orders.
        std::vector<UnitCommand>& queue = m_commandQueues[unitId];
        if (queue.size() < MAX_QUEUED_COMMANDS)
        {
            queue.push_back(command);
            queue.back().path.clear();
        }
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

    const std::map<uint32_t, std::vector<UnitCommand>>& RTSCommandSystem::GetCommandQueues() const
    {
        return m_commandQueues;
    }

    bool RTSCommandSystem::RestoreRuntimeState(const std::map<uint32_t, std::vector<UnitCommand>>& queues,
                                               const std::vector<uint32_t>& selection)
    {
        for (const auto& [unitId, queue] : queues)
        {
            if (unitId == 0 || queue.empty() || queue.size() > MAX_QUEUED_COMMANDS ||
                !std::ranges::all_of(queue, IsCommandValid))
                return false;
        }
        std::vector<uint32_t> sortedSelection = selection;
        std::ranges::sort(sortedSelection);
        if (std::ranges::find(sortedSelection, 0u) != sortedSelection.end() ||
            std::ranges::adjacent_find(sortedSelection) != sortedSelection.end())
        {
            return false;
        }

        m_commandQueues = queues;
        m_selectedUnits = selection;
        return true;
    }

    // === Internal ===

    void RTSCommandSystem::ProcessCommands(float deltaTime)
    {
        if (!m_unitSystem)
            return;

        PruneSelection();
        m_obstaclesCurrent = false;
        const float safeDeltaTime = std::isfinite(deltaTime) && deltaTime > 0.0f ? deltaTime : 0.0f;

        for (auto it = m_commandQueues.begin(); it != m_commandQueues.end();)
        {
            UnitData* unit = m_unitSystem->GetUnitMutable(it->first);
            if (!unit || unit->state == RTSUnitState::Dead)
            {
                it = m_commandQueues.erase(it);
                continue;
            }

            auto& queue = it->second;

            if (queue.empty())
            {
                it = m_commandQueues.erase(it);
                continue;
            }

            auto& cmd = queue.front();
            switch (cmd.type)
            {
            case RTSCommandType::Move:
            case RTSCommandType::Attack:
            {
                if (cmd.type == RTSCommandType::Attack && cmd.targetEntity != 0)
                {
                    // Targeted attack: combat resolution owns the engagement from here.
                    unit->state = RTSUnitState::Attacking;
                    unit->targetId = cmd.targetEntity;
                    queue.erase(queue.begin());
                    break;
                }

                // Move, or attack-move: an attack-move holds position while combat reports an engagement.
                const bool attackMove = cmd.type == RTSCommandType::Attack;
                if (attackMove && unit->state == RTSUnitState::Attacking && unit->targetId != 0)
                    break;

                unit->state = attackMove ? RTSUnitState::Attacking : RTSUnitState::Moving;
                unit->targetId = 0;
                if (AdvanceAlongRoute(*unit, cmd, safeDeltaTime))
                {
                    unit->state = RTSUnitState::Idle;
                    queue.erase(queue.begin());
                }
                break;
            }
            case RTSCommandType::Stop:
                unit->state = RTSUnitState::Idle;
                unit->targetId = 0;
                queue.clear();
                break;
            case RTSCommandType::Hold:
                unit->state = RTSUnitState::Holding;
                unit->targetId = 0;
                queue.erase(queue.begin());
                break;
            case RTSCommandType::Gather:
                unit->state = RTSUnitState::Gathering;
                unit->targetId = cmd.targetEntity;
                queue.erase(queue.begin());
                break;
            case RTSCommandType::Build:
                unit->state = RTSUnitState::Building;
                unit->targetId = cmd.targetEntity;
                queue.erase(queue.begin());
                break;
            case RTSCommandType::Patrol:
                unit->state = RTSUnitState::Patrolling;
                unit->targetId = 0;
                queue.erase(queue.begin());
                break;
            default:
                queue.clear();
                break;
            }

            if (queue.empty())
                it = m_commandQueues.erase(it);
            else
                ++it;
        }
    }

    bool RTSCommandSystem::AdvanceAlongRoute(UnitData& unit, UnitCommand& command, float deltaTime)
    {
        if (!m_obstaclesCurrent)
        {
            if (m_buildingSystem)
                m_pathfinder.RebuildObstacles(*m_buildingSystem);
            else
                m_pathfinder.ClearObstacles();
            m_obstaclesCurrent = true;
        }

        // Plan when the order becomes current, and again if a structure now stands across the remaining route.
        if (command.path.empty() || !m_pathfinder.IsRouteClear(unit.posX, unit.posY, command.path))
        {
            command.path = m_pathfinder.FindPath(unit.posX, unit.posY, command.targetX, command.targetY);
            if (command.path.empty())
                return true; // Walled in: nothing to walk towards
        }

        const float speed = std::isfinite(unit.moveSpeed) ? std::max(unit.moveSpeed, 0.0f) : 0.0f;
        float remaining = speed * deltaTime;
        while (!command.path.empty())
        {
            const RTSWaypoint waypoint = command.path.front();
            const float dx = waypoint.x - unit.posX;
            const float dy = waypoint.y - unit.posY;
            // sqrt is correctly rounded on every IEEE-754 platform; std::hypot is not, which breaks lockstep.
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance <= 0.001f || (remaining > 0.0f && remaining >= distance))
            {
                // Reach the waypoint and spend what is left of this tick's movement on the next leg.
                unit.posX = waypoint.x;
                unit.posY = waypoint.y;
                remaining = remaining > distance ? remaining - distance : 0.0f;
                command.path.erase(command.path.begin());
                continue;
            }
            if (remaining > 0.0f)
            {
                unit.posX += dx / distance * remaining;
                unit.posY += dy / distance * remaining;
            }
            break;
        }
        return command.path.empty();
    }

    bool RTSCommandSystem::IsCommandValid(const UnitCommand& command)
    {
        if (command.type >= RTSCommandType::Count)
            return false;

        const bool routed = command.type == RTSCommandType::Move ||
                            (command.type == RTSCommandType::Attack && command.targetEntity == 0);
        if (!command.path.empty() &&
            (!routed || command.path.size() > RTSGridPathfinder::MAX_WAYPOINTS ||
             !std::ranges::all_of(command.path, [](const RTSWaypoint& waypoint)
                                  { return std::isfinite(waypoint.x) && std::isfinite(waypoint.y); })))
        {
            return false;
        }

        if (command.type == RTSCommandType::Move || command.type == RTSCommandType::Patrol ||
            command.type == RTSCommandType::Build ||
            (command.type == RTSCommandType::Attack && command.targetEntity == 0))
        {
            return std::isfinite(command.targetX) && std::isfinite(command.targetY);
        }
        return true;
    }

    void RTSCommandSystem::PruneSelection()
    {
        std::erase_if(m_selectedUnits,
                      [this](uint32_t unitId)
                      {
                          const UnitData* unit = m_unitSystem->GetUnit(unitId);
                          return !unit || unit->state == RTSUnitState::Dead;
                      });
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
