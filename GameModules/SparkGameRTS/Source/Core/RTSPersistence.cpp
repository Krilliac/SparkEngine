/**
 * @file RTSPersistence.cpp
 * @brief Full-state RTS skirmish snapshot: capture, validation, and all-or-nothing apply.
 */

#include "RTSPersistence.h"

#include "Simulation/RTSSkirmishSimulation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace RTS
{
    namespace
    {
        constexpr size_t MaxRecords = RTSPersistence::MAX_RECORDS;
        constexpr size_t MaxPlayerEconomies = static_cast<size_t>(RTSFaction::Count);
        constexpr size_t MaxProductionQueue = RTSPersistence::MAX_PRODUCTION_QUEUE;

        template <typename... Values> bool Finite(Values... values)
        {
            return (... && std::isfinite(static_cast<double>(values)));
        }

        bool Fail(std::string& error, std::string message)
        {
            error = std::move(message);
            return false;
        }

        bool ValidateUnits(const RTSPersistenceSnapshot& snapshot,
                           std::unordered_map<uint32_t, const UnitData*>& unitsById, std::string& error)
        {
            unitsById.reserve(snapshot.units.size());
            for (const UnitData& unit : snapshot.units)
            {
                const std::string id = std::to_string(unit.unitId);
                if (unit.unitId == 0 || unit.unitId >= snapshot.nextUnitId)
                    return Fail(error, "invalid unit identifier " + id);
                if (unit.type >= RTSUnitType::Count || unit.faction >= RTSFaction::Count ||
                    unit.state >= RTSUnitState::Count)
                    return Fail(error, "invalid unit enum value for id " + id);
                if (!Finite(unit.health, unit.maxHealth, unit.damage, unit.attackSpeed, unit.moveSpeed,
                            unit.visionRange, unit.posX, unit.posY))
                    return Fail(error, "non-finite unit field for id " + id);
                if (unit.maxHealth <= 0.0f || unit.health < 0.0f || unit.health > unit.maxHealth ||
                    unit.damage < 0.0f || unit.attackSpeed < 0.0f || unit.moveSpeed < 0.0f || unit.visionRange < 0.0f)
                    return Fail(error, "invalid unit stat range for id " + id);
                if (!unitsById.emplace(unit.unitId, &unit).second)
                    return Fail(error, "duplicate unit identifier " + id);
            }
            return true;
        }

        bool ValidateBuildings(const RTSPersistenceSnapshot& snapshot, std::string& error)
        {
            std::unordered_set<uint32_t> buildingIds;
            for (const BuildingData& building : snapshot.buildings)
            {
                if (building.buildingId == 0 || building.buildingId >= snapshot.nextBuildingId ||
                    building.type >= RTSBuildingType::Count || building.faction >= RTSFaction::Count ||
                    !Finite(building.health, building.maxHealth, building.posX, building.posY,
                            building.constructionProgress, building.constructionTime) ||
                    building.maxHealth <= 0.0f || building.health < 0.0f || building.health > building.maxHealth ||
                    building.constructionProgress < 0.0f || building.constructionProgress > 1.0f ||
                    building.constructionTime <= 0.0f || building.productionQueue.size() > MaxProductionQueue ||
                    !buildingIds.insert(building.buildingId).second)
                    return Fail(error, "invalid or duplicate building record");
                for (const ProductionEntry& entry : building.productionQueue)
                {
                    if (entry.unitType >= RTSUnitType::Count || !Finite(entry.timeRemaining, entry.totalTime) ||
                        entry.totalTime <= 0.0f || entry.timeRemaining < 0.0f || entry.timeRemaining > entry.totalTime)
                        return Fail(error, "invalid production queue entry");
                }
            }
            return true;
        }

        bool ValidateEconomy(const RTSPersistenceSnapshot& snapshot,
                             const std::unordered_map<uint32_t, const UnitData*>& unitsById, std::string& error)
        {
            std::unordered_set<uint8_t> factions;
            for (const auto& [faction, resources] : snapshot.players)
            {
                if (faction >= RTSFaction::Count || resources.minerals < 0 || resources.gas < 0 ||
                    resources.currentSupply < 0 || resources.maxSupply < 0 ||
                    resources.currentSupply > resources.maxSupply ||
                    !factions.insert(static_cast<uint8_t>(faction)).second)
                    return Fail(error, "invalid or duplicate player economy record");
            }
            if (!Finite(snapshot.gatherTimer) || snapshot.gatherTimer < 0.0f ||
                snapshot.gatherTimer >= RTSResourceSystem::GATHER_INTERVAL)
                return Fail(error, "invalid harvest timer");

            std::unordered_set<uint32_t> nodeIds;
            for (const ResourceNode& node : snapshot.resourceNodes)
            {
                if (node.nodeId == 0 || node.nodeId >= snapshot.nextNodeId ||
                    (node.type != RTSResourceType::Minerals && node.type != RTSResourceType::Gas) ||
                    !Finite(node.posX, node.posY) || node.remaining < 0 || node.maxWorkers <= 0 ||
                    node.assignedWorkers.size() > static_cast<size_t>(node.maxWorkers) ||
                    !nodeIds.insert(node.nodeId).second)
                    return Fail(error, "invalid or duplicate resource node");

                // A worker killed this tick stays assigned until the next harvest prunes it, so an id may name a
                // unit that no longer exists; one that does exist must still be a worker.
                std::unordered_set<uint32_t> assigned;
                for (uint32_t workerId : node.assignedWorkers)
                {
                    const auto worker = unitsById.find(workerId);
                    if (workerId == 0 || workerId >= snapshot.nextUnitId || !assigned.insert(workerId).second ||
                        (worker != unitsById.end() && worker->second->type != RTSUnitType::Worker))
                        return Fail(error, "resource node references an invalid worker");
                }
            }
            return true;
        }

        bool ValidateOrders(const RTSPersistenceSnapshot& snapshot, std::string& error)
        {
            if (snapshot.commandQueues.size() > MaxRecords || snapshot.selection.size() > MaxRecords)
                return Fail(error, "command record limit exceeded");
            // Queues and selection entries of units killed this tick survive until the next command update.
            for (const auto& [unitId, queue] : snapshot.commandQueues)
            {
                if (unitId == 0 || unitId >= snapshot.nextUnitId || queue.empty() ||
                    queue.size() > RTSCommandSystem::MAX_QUEUED_COMMANDS ||
                    !std::ranges::all_of(queue, RTSCommandSystem::IsCommandValid))
                    return Fail(error, "invalid command queue for unit " + std::to_string(unitId));
            }
            std::unordered_set<uint32_t> selected;
            for (uint32_t unitId : snapshot.selection)
            {
                if (unitId == 0 || unitId >= snapshot.nextUnitId || !selected.insert(unitId).second)
                    return Fail(error, "invalid or duplicate selection entry");
            }
            return true;
        }

        bool ValidateMatchAndFog(const RTSPersistenceSnapshot& snapshot, std::string& error)
        {
            const RTSMatchSnapshot& match = snapshot.match;
            if (match.state >= RTSMatchState::Count || match.winner >= RTSFaction::Count || !Finite(match.matchTime) ||
                match.matchTime < 0.0f || match.players.size() > static_cast<size_t>(RTSMatchSystem::MAX_PLAYERS))
                return Fail(error, "invalid match state");
            for (const PlayerSetup& player : match.players)
            {
                if (player.faction >= RTSFaction::Count || !Finite(player.startX, player.startY))
                    return Fail(error, "invalid match player");
            }

            if (snapshot.fog.size() != static_cast<size_t>(RTSFaction::Count))
                return Fail(error, "fog of war must hold one grid per faction");
            const int width = snapshot.fog.front().width;
            const int height = snapshot.fog.front().height;
            if (width <= 0 || height <= 0 || width > RTSFogOfWarSystem::MAX_MAP_DIMENSION ||
                height > RTSFogOfWarSystem::MAX_MAP_DIMENSION)
                return Fail(error, "invalid fog of war dimensions");
            for (const FogGrid& grid : snapshot.fog)
            {
                if (grid.width != width || grid.height != height ||
                    grid.cells.size() != static_cast<size_t>(width) * static_cast<size_t>(height) ||
                    !std::ranges::all_of(grid.cells, [](RTSVisibility cell) { return cell < RTSVisibility::Count; }))
                    return Fail(error, "invalid fog of war grid");
            }
            return true;
        }

    } // namespace

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

    bool RTSPersistence::Validate(const RTSPersistenceSnapshot& snapshot, std::string& error)
    {
        if (snapshot.units.size() > MaxRecords || snapshot.buildings.size() > MaxRecords ||
            snapshot.resourceNodes.size() > MaxRecords || snapshot.players.size() > MaxPlayerEconomies)
            return Fail(error, "snapshot record limit exceeded");
        if (snapshot.nextUnitId == 0 || snapshot.nextBuildingId == 0 || snapshot.nextNodeId == 0)
            return Fail(error, "invalid identifier counter");

        std::unordered_map<uint32_t, const UnitData*> unitsById;
        if (!ValidateUnits(snapshot, unitsById, error) || !ValidateBuildings(snapshot, error) ||
            !ValidateEconomy(snapshot, unitsById, error) || !ValidateOrders(snapshot, error) ||
            !ValidateMatchAndFog(snapshot, error))
            return false;

        error.clear();
        return true;
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
            return Fail(error, rolledBack ? "RTS systems rejected a validated snapshot"
                                          : "RTS systems rejected a validated snapshot; rollback incomplete");
        }

        simulation.RestoreClock(snapshot.tick);
        error.clear();
        return true;
    }
} // namespace RTS
