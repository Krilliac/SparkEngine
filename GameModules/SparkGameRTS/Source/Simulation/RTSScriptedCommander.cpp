/**
 * @file RTSScriptedCommander.cpp
 * @brief Tick-scheduled Human player for automated skirmishes (see RTSScriptedCommander.h)
 */

#include "RTSScriptedCommander.h"

#include "Building/RTSBuildingSystem.h"
#include "Command/RTSCommandSystem.h"
#include "RTSSkirmishSimulation.h"
#include "Unit/RTSUnitSystem.h"

#include <vector>

namespace RTS
{
    static_assert(static_cast<float>(RTSScriptedCommander::TICKS_PER_SECOND) * RTSSkirmishSimulation::TICK_SECONDS ==
                      1.0f,
                  "the commander schedules orders in simulation ticks");

    void RTSScriptedCommander::Apply(const RTSSkirmishSystems& systems, uint64_t tick) const
    {
        RTSUnitSystem& units = *systems.units;
        RTSBuildingSystem& buildings = *systems.buildings;
        RTSCommandSystem& commands = *systems.commands;

        const auto humanBarracks = [&]() -> uint32_t
        {
            for (uint32_t id : buildings.GetBuildingsByFaction(RTSFaction::Human))
            {
                if (buildings.GetBuilding(id)->type == RTSBuildingType::Barracks)
                    return id;
            }
            return 0;
        };

        if (tick == 0 || tick == TICKS_PER_SECOND * 20 || tick == TICKS_PER_SECOND * 40)
            buildings.StartProduction(humanBarracks(), RTSUnitType::Marine);
        if (tick == COUNTER_ATTACK_TICK)
        {
            commands.DeselectAll();
            for (uint32_t id : units.GetUnitsByFaction(RTSFaction::Human))
            {
                if (units.GetUnit(id)->type != RTSUnitType::Worker)
                    commands.AddToSelection(id);
            }
            commands.IssueCommandToSelection({RTSCommandType::Attack, 60.0f, 60.0f, 0});
        }
        if (tick > COUNTER_ATTACK_TICK && tick % TICKS_PER_SECOND == 0)
        {
            const std::vector<uint32_t> targets = buildings.GetBuildingsByFaction(RTSFaction::Swarm);
            const std::vector<uint32_t> survivors = units.GetUnitsByFaction(RTSFaction::Swarm);
            float x = 0.0f;
            float y = 0.0f;
            if (!targets.empty())
            {
                x = buildings.GetBuilding(targets.front())->posX;
                y = buildings.GetBuilding(targets.front())->posY;
            }
            else if (!survivors.empty())
            {
                x = units.GetUnit(survivors.front())->posX;
                y = units.GetUnit(survivors.front())->posY;
            }
            else
            {
                return;
            }
            for (uint32_t id : units.GetUnitsByFaction(RTSFaction::Human))
            {
                const UnitData* unit = units.GetUnit(id);
                if (unit->type != RTSUnitType::Worker && unit->state == RTSUnitState::Idle &&
                    !commands.GetCurrentCommand(id))
                {
                    commands.IssueCommand(id, {RTSCommandType::Attack, x, y, 0});
                }
            }
        }
    }
} // namespace RTS
