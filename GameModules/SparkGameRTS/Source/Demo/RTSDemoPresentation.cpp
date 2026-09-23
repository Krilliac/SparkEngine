/**
 * @file RTSDemoPresentation.cpp
 * @brief Playable RTS showcase setup, controls, fog updates, and battlefield UI
 */

#include "RTSDemoPresentation.h"

#include "Building/RTSBuildingSystem.h"
#include "Command/RTSCommandSystem.h"
#include "FogOfWar/RTSFogOfWarSystem.h"
#include "Input/InputManager.h"
#include "Match/RTSMatchSystem.h"
#include "Resource/RTSResourceSystem.h"
#include "Simulation/RTSSkirmishSimulation.h"
#include "Unit/RTSUnitSystem.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>

namespace RTS
{
    namespace
    {
        constexpr RTSFaction PLAYER_FACTION = RTSFaction::Human;
        constexpr int DEMO_MAP_SIZE = RTSSkirmishSimulation::MAP_SIZE;

#ifdef ENABLE_EDITOR
        ImU32 GetFactionColor(RTSFaction faction)
        {
            switch (faction)
            {
            case RTSFaction::Human:
                return IM_COL32(72, 168, 255, 255);
            case RTSFaction::Sentinel:
                return IM_COL32(255, 190, 72, 255);
            case RTSFaction::Swarm:
                return IM_COL32(239, 82, 112, 255);
            default:
                return IM_COL32(210, 210, 210, 255);
            }
        }
#endif
    } // namespace

    bool RTSDemoPresentation::Initialize(Spark::IEngineContext* context, RTSUnitSystem* units,
                                         RTSBuildingSystem* buildings, RTSResourceSystem* resources,
                                         RTSCommandSystem* commands, RTSFogOfWarSystem* fog, RTSMatchSystem* match,
                                         RTSSkirmishSimulation* simulation)
    {
        m_context = context;
        m_units = units;
        m_buildings = buildings;
        m_resources = resources;
        m_commands = commands;
        m_fog = fog;
        m_match = match;
        m_simulation = simulation;
        return Reset();
    }

    void RTSDemoPresentation::Shutdown()
    {
        m_context = nullptr;
        m_units = nullptr;
        m_buildings = nullptr;
        m_resources = nullptr;
        m_commands = nullptr;
        m_fog = nullptr;
        m_match = nullptr;
        m_simulation = nullptr;
    }

    bool RTSDemoPresentation::Reset()
    {
        if (!m_units || !m_buildings || !m_resources || !m_commands || !m_fog || !m_match || !m_simulation ||
            !m_simulation->StartDefaultSkirmish())
        {
            return false;
        }

        SelectUnitType(RTSUnitType::Marine);
        m_waypointIndex = 0;
        return true;
    }

    bool RTSDemoPresentation::IsPressed(int key, bool& heldState) const
    {
        InputManager* input = m_context ? m_context->GetInput() : nullptr;
        const bool isDown = input && input->IsKeyDown(key);
        const bool pressed = isDown && !heldState;
        heldState = isDown;
        return pressed;
    }

    void RTSDemoPresentation::UpdateInput()
    {
        if (IsPressed('1', m_workerHeld))
            SelectUnitType(RTSUnitType::Worker);
        if (IsPressed('2', m_marineHeld))
            SelectUnitType(RTSUnitType::Marine);
        if (IsPressed('3', m_tankHeld))
            SelectUnitType(RTSUnitType::Tank);
        if (IsPressed('H', m_holdHeld))
            HoldSelection();
        if (IsPressed('S', m_stopHeld))
            StopSelection();
        if (IsPressed('R', m_restartHeld))
            Reset();

        if (IsPressed('M', m_moveHeld))
        {
            static constexpr std::array<std::array<float, 2>, 4> waypoints = {
                {{{42.0f, 32.0f}}, {{59.0f, 43.0f}}, {{70.0f, 60.0f}}, {{31.0f, 52.0f}}}};
            const auto& waypoint = waypoints[m_waypointIndex % waypoints.size()];
            MoveSelection(waypoint[0], waypoint[1]);
            ++m_waypointIndex;
        }
    }

    void RTSDemoPresentation::SelectUnitType(RTSUnitType type)
    {
        if (m_commands)
            m_commands->SelectAllOfType(type, PLAYER_FACTION);
    }

    void RTSDemoPresentation::SelectArmy()
    {
        if (!m_units || !m_commands)
            return;
        m_commands->DeselectAll();
        for (uint32_t unitId : m_units->GetUnitsByFaction(PLAYER_FACTION))
        {
            const UnitData* unit = m_units->GetUnit(unitId);
            if (unit && unit->type != RTSUnitType::Worker)
                m_commands->AddToSelection(unitId);
        }
    }

    bool RTSDemoPresentation::MoveSelection(float x, float y, bool queued)
    {
        if (!m_commands || !std::isfinite(x) || !std::isfinite(y))
            return false;
        x = std::clamp(x, 0.0f, static_cast<float>(DEMO_MAP_SIZE - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(DEMO_MAP_SIZE - 1));
        const auto selection = m_commands->GetSelection();
        if (selection.empty())
            return false;
        const UnitCommand command{RTSCommandType::Move, x, y, 0};
        for (uint32_t unitId : selection)
        {
            if (queued)
                m_commands->QueueCommand(unitId, command);
            else
                m_commands->IssueCommand(unitId, command);
        }
        return true;
    }

    bool RTSDemoPresentation::HoldSelection()
    {
        if (!m_commands || m_commands->GetSelection().empty())
            return false;
        m_commands->IssueCommandToSelection({RTSCommandType::Hold, 0.0f, 0.0f, 0});
        return true;
    }

    bool RTSDemoPresentation::StopSelection()
    {
        if (!m_commands || m_commands->GetSelection().empty())
            return false;
        m_commands->IssueCommandToSelection({RTSCommandType::Stop, 0.0f, 0.0f, 0});
        return true;
    }

    bool RTSDemoPresentation::TrainMarine()
    {
        if (!m_buildings)
            return false;
        for (uint32_t buildingId : m_buildings->GetBuildingsByFaction(PLAYER_FACTION))
        {
            const BuildingData* building = m_buildings->GetBuilding(buildingId);
            if (building && building->type == RTSBuildingType::Barracks && building->constructionComplete)
                return m_buildings->StartProduction(buildingId, RTSUnitType::Marine);
        }
        return false;
    }

    void RTSDemoPresentation::RenderUI()
    {
#ifdef ENABLE_EDITOR
        if (!m_units || !m_buildings || !m_resources || !m_commands || !m_fog || !m_match)
            return;
        if (ImGui::Begin("RTS Battlefield"))
        {
            if (const PlayerResources* resources = m_resources->GetPlayerResources(PLAYER_FACTION))
            {
                ImGui::Text("Minerals %d   Gas %d   Supply %d/%d", resources->minerals, resources->gas,
                            resources->currentSupply, resources->maxSupply);
            }
            ImGui::SameLine();
            ImGui::Text("Time %.1fs", m_match->GetMatchTime());
            if (ImGui::Button("Select army"))
                SelectArmy();
            ImGui::SameLine();
            if (ImGui::Button("Hold"))
                HoldSelection();
            ImGui::SameLine();
            if (ImGui::Button("Stop"))
                StopSelection();
            ImGui::SameLine();
            if (ImGui::Button("Train marine"))
                TrainMarine();
            ImGui::SameLine();
            if (ImGui::Button("Restart demo"))
                Reset();
            ImGui::TextDisabled("Left click selects | Right click moves | 1 workers | 2 marines | 3 tanks | M waypoint "
                                "| H hold | S stop | R restart");
            DrawBattlefield();
        }
        ImGui::End();
#endif
    }

    void RTSDemoPresentation::DrawBattlefield()
    {
#ifdef ENABLE_EDITOR
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float extent = std::clamp(std::min(available.x, available.y), 280.0f, 680.0f);
        const ImVec2 canvasSize(extent, extent);
        const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##rts_battlefield", canvasSize,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 canvasEnd(canvasPos.x + extent, canvasPos.y + extent);
        drawList->AddRectFilled(canvasPos, canvasEnd, IM_COL32(17, 25, 31, 255), 5.0f);
        drawList->PushClipRect(canvasPos, canvasEnd, true);

        const auto toScreen = [&](float x, float y)
        {
            return ImVec2(canvasPos.x + x / static_cast<float>(DEMO_MAP_SIZE) * extent,
                          canvasPos.y + (1.0f - y / static_cast<float>(DEMO_MAP_SIZE)) * extent);
        };
        const auto toWorld = [&](const ImVec2& point)
        {
            return std::array<float, 2>{std::clamp((point.x - canvasPos.x) / extent * DEMO_MAP_SIZE, 0.0f,
                                                   static_cast<float>(DEMO_MAP_SIZE - 1)),
                                        std::clamp((1.0f - (point.y - canvasPos.y) / extent) * DEMO_MAP_SIZE, 0.0f,
                                                   static_cast<float>(DEMO_MAP_SIZE - 1))};
        };
        for (int grid = 0; grid <= DEMO_MAP_SIZE; grid += 8)
        {
            const ImVec2 vertical = toScreen(static_cast<float>(grid), 0.0f);
            const ImVec2 horizontal = toScreen(0.0f, static_cast<float>(grid));
            drawList->AddLine(ImVec2(vertical.x, canvasPos.y), ImVec2(vertical.x, canvasEnd.y),
                              IM_COL32(70, 91, 100, 65));
            drawList->AddLine(ImVec2(canvasPos.x, horizontal.y), ImVec2(canvasEnd.x, horizontal.y),
                              IM_COL32(70, 91, 100, 65));
        }
        for (const auto& [nodeId, node] : m_resources->GetNodes())
        {
            (void)nodeId;
            if (!m_fog->IsExplored(PLAYER_FACTION, node.posX, node.posY))
                continue;
            const ImVec2 center = toScreen(node.posX, node.posY);
            const ImU32 color =
                node.type == RTSResourceType::Minerals ? IM_COL32(79, 218, 255, 255) : IM_COL32(87, 235, 143, 255);
            drawList->AddQuadFilled(ImVec2(center.x, center.y - 6.0f), ImVec2(center.x + 6.0f, center.y),
                                    ImVec2(center.x, center.y + 6.0f), ImVec2(center.x - 6.0f, center.y), color);
        }
        for (int factionIndex = 0; factionIndex < static_cast<int>(RTSFaction::Count); ++factionIndex)
        {
            const auto faction = static_cast<RTSFaction>(factionIndex);
            for (uint32_t buildingId : m_buildings->GetBuildingsByFaction(faction))
            {
                const BuildingData* building = m_buildings->GetBuilding(buildingId);
                if (!building ||
                    (faction != PLAYER_FACTION && !m_fog->IsVisible(PLAYER_FACTION, building->posX, building->posY)))
                    continue;
                const ImVec2 center = toScreen(building->posX, building->posY);
                drawList->AddRectFilled(ImVec2(center.x - 8.0f, center.y - 8.0f),
                                        ImVec2(center.x + 8.0f, center.y + 8.0f), GetFactionColor(faction), 2.0f);
                drawList->AddRect(ImVec2(center.x - 9.0f, center.y - 9.0f), ImVec2(center.x + 9.0f, center.y + 9.0f),
                                  IM_COL32(230, 240, 245, 210), 2.0f);
            }
            for (uint32_t unitId : m_units->GetUnitsByFaction(faction))
            {
                const UnitData* unit = m_units->GetUnit(unitId);
                if (!unit || (faction != PLAYER_FACTION && !m_fog->IsVisible(PLAYER_FACTION, unit->posX, unit->posY)))
                    continue;
                const ImVec2 center = toScreen(unit->posX, unit->posY);
                const float radius = unit->type == RTSUnitType::Tank ? 6.0f : 4.5f;
                drawList->AddCircleFilled(center, radius, GetFactionColor(faction), 12);
                const auto& selection = m_commands->GetSelection();
                if (std::ranges::find(selection, unitId) != selection.end())
                    drawList->AddCircle(center, radius + 3.0f, IM_COL32(255, 244, 126, 255), 16, 2.0f);
            }
        }
        constexpr int fogStep = 8;
        for (int y = 0; y < DEMO_MAP_SIZE; y += fogStep)
        {
            for (int x = 0; x < DEMO_MAP_SIZE; x += fogStep)
            {
                const RTSVisibility visibility =
                    m_fog->GetVisibilityAtPosition(PLAYER_FACTION, x + fogStep * 0.5f, y + fogStep * 0.5f);
                if (visibility == RTSVisibility::Visible)
                    continue;
                const ImVec2 topLeft = toScreen(static_cast<float>(x), static_cast<float>(y + fogStep));
                const ImVec2 bottomRight = toScreen(static_cast<float>(x + fogStep), static_cast<float>(y));
                drawList->AddRectFilled(topLeft, bottomRight,
                                        visibility == RTSVisibility::Fog ? IM_COL32(6, 9, 12, 125)
                                                                         : IM_COL32(2, 4, 7, 225));
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            const auto world = toWorld(ImGui::GetMousePos());
            uint32_t nearestUnit = 0;
            float nearestDistance = 3.5f;
            for (uint32_t unitId : m_units->GetUnitsByFaction(PLAYER_FACTION))
            {
                const UnitData* unit = m_units->GetUnit(unitId);
                if (!unit)
                    continue;
                const float distance = std::hypot(unit->posX - world[0], unit->posY - world[1]);
                if (distance < nearestDistance)
                {
                    nearestDistance = distance;
                    nearestUnit = unitId;
                }
            }
            if (nearestUnit != 0)
            {
                if (ImGui::GetIO().KeyShift)
                    m_commands->AddToSelection(nearestUnit);
                else
                    m_commands->Select(nearestUnit);
            }
            else if (!ImGui::GetIO().KeyShift)
                m_commands->DeselectAll();
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            const auto world = toWorld(ImGui::GetMousePos());
            MoveSelection(world[0], world[1], ImGui::GetIO().KeyShift);
        }
        drawList->PopClipRect();
#endif
    }

} // namespace RTS
