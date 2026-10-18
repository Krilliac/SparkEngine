/**
 * @file RTSPersistence.cpp
 * @brief Full-state RTS skirmish snapshot: capture and all-or-nothing apply (Validate lives in
 *        RTSPersistenceValidation.cpp, which needs none of the live systems).
 */

#include "RTSPersistence.h"

#include "Simulation/RTSSkirmishSimulation.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace RTS
{
    bool RTSPersistence::IsValidSlotName(std::string_view slotName)
    {
        if (slotName.empty() || slotName.size() > 64)
            return false;
        return std::ranges::all_of(slotName, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    }

    RTSPersistenceSnapshot RTSPersistence::Capture(const RTSUnitSystem& units, const RTSBuildingSystem& buildings,
                                                   const RTSResourceSystem& resources)
    {
        RTSPersistenceSnapshot snapshot;
        for (uint8_t value = 0; value < static_cast<uint8_t>(RTSFaction::Count); ++value)
        {
            const auto faction = static_cast<RTSFaction>(value);
            for (uint32_t id : units.GetUnitsByFaction(faction))
            {
                if (const UnitData* unit = units.GetUnit(id))
                    snapshot.units.push_back(*unit);
            }
            for (uint32_t id : buildings.GetBuildingsByFaction(faction))
            {
                if (const BuildingData* building = buildings.GetBuilding(id))
                    snapshot.buildings.push_back(*building);
            }
            if (const PlayerResources* player = resources.GetPlayerResources(faction))
                snapshot.players.emplace_back(faction, *player);
        }

        // Nodes come out of an id-ordered map. Worker order is kept as-is: it decides which worker harvests
        // the last minerals of a node, so sorting it would change the resumed simulation.
        for (const auto& [id, node] : resources.GetNodes())
        {
            (void)id;
            snapshot.resourceNodes.push_back(node);
        }

        std::ranges::sort(snapshot.units, {}, &UnitData::unitId);
        std::ranges::sort(snapshot.buildings, {}, &BuildingData::buildingId);
        snapshot.nextUnitId = units.GetNextUnitId();
        snapshot.nextBuildingId = buildings.GetNextBuildingId();
        snapshot.nextNodeId = resources.GetNextNodeId();
        snapshot.gatherTimer = resources.GetGatherTimer();
        return snapshot;
    }

    RTSPersistenceSnapshot RTSPersistence::Capture(const RTSSkirmishSystems& systems,
                                                   const RTSSkirmishSimulation& simulation)
    {
        RTSPersistenceSnapshot snapshot = Capture(*systems.units, *systems.buildings, *systems.resources);
        snapshot.commandQueues = systems.commands->GetCommandQueues();
        snapshot.selection = systems.commands->GetSelection();
        snapshot.match = systems.match->CaptureState();
        for (uint8_t value = 0; value < static_cast<uint8_t>(RTSFaction::Count); ++value)
        {
            const FogGrid* grid = systems.fog->GetGrid(static_cast<RTSFaction>(value));
            snapshot.fog.push_back(grid ? *grid : FogGrid{});
        }
        snapshot.tick = simulation.GetTick();
        return snapshot;
    }

    bool RTSPersistence::Apply(const RTSPersistenceSnapshot& snapshot, const RTSSkirmishSystems& systems,
                               RTSSkirmishSimulation& simulation, std::string& error)
    {
        if (!Validate(snapshot, error))
            return false;

        auto& [units, buildings, resources, commands, fog, match] = systems;
        const RTSPersistenceSnapshot previous = Capture(systems, simulation);
        if (!units->RestoreState(snapshot.units, snapshot.nextUnitId) ||
            !resources->RestoreState(snapshot.players, snapshot.resourceNodes, snapshot.nextNodeId,
                                     snapshot.gatherTimer) ||
            !buildings->RestoreState(snapshot.buildings, snapshot.nextBuildingId) ||
            !commands->RestoreRuntimeState(snapshot.commandQueues, snapshot.selection) ||
            !match->RestoreState(snapshot.match) || !fog->RestoreState(snapshot.fog))
        {
            // Defensive guard: Validate mirrors every RestoreState check, so this branch is reached only if the
            // two drift apart. Roll every system back independently and say so when a rollback is refused too
            // (for example fog captured before it was initialized), since the match is then not the pre-call one.
            bool rolledBack = units->RestoreState(previous.units, previous.nextUnitId);
            rolledBack &= resources->RestoreState(previous.players, previous.resourceNodes, previous.nextNodeId,
                                                  previous.gatherTimer);
            rolledBack &= buildings->RestoreState(previous.buildings, previous.nextBuildingId);
            rolledBack &= commands->RestoreRuntimeState(previous.commandQueues, previous.selection);
            rolledBack &= match->RestoreState(previous.match);
            rolledBack &= fog->RestoreState(previous.fog);
            error = rolledBack ? "RTS systems rejected a validated snapshot"
                               : "RTS systems rejected a validated snapshot; rollback incomplete";
            return false;
        }

        simulation.RestoreClock(snapshot.tick);
        error.clear();
        return true;
    }
} // namespace RTS
