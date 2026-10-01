/**
 * @file RTSPersistenceValidation.cpp
 * @brief RTSPersistence::Validate: the snapshot checks Apply and every restoring system rely on.
 *
 * Kept apart from RTSPersistence.cpp (Capture and Apply, which drive the live systems) so the
 * SEC-120 save-snapshot fuzz target (FuzzerTests/FuzzRTSPersistence.cpp) links the decoder and
 * this validator without the unit, building, resource, match, fog and pathfinding systems.
 */

#include "RTSPersistence.h"

#include <algorithm>
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
                {
                    return Fail(error, "invalid unit identifier " + id);
                }
                if (unit.type >= RTSUnitType::Count || unit.faction >= RTSFaction::Count ||
                    unit.state >= RTSUnitState::Count)
                {
                    return Fail(error, "invalid unit enum value for id " + id);
                }
                if (!Finite(unit.health, unit.maxHealth, unit.damage, unit.attackSpeed, unit.moveSpeed,
                            unit.visionRange, unit.posX, unit.posY))
                {
                    return Fail(error, "non-finite unit field for id " + id);
                }
                if (unit.maxHealth <= 0.0f || unit.health < 0.0f || unit.health > unit.maxHealth ||
                    unit.damage < 0.0f || unit.attackSpeed < 0.0f || unit.moveSpeed < 0.0f || unit.visionRange < 0.0f ||
                    unit.visionRange > RTSUnitSystem::MAX_VISION_RANGE)
                {
                    return Fail(error, "invalid unit stat range for id " + id);
                }
                if (!unitsById.emplace(unit.unitId, &unit).second)
                {
                    return Fail(error, "duplicate unit identifier " + id);
                }
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
                {
                    return Fail(error, "invalid or duplicate building record");
                }
                for (const ProductionEntry& entry : building.productionQueue)
                {
                    if (entry.unitType >= RTSUnitType::Count || !Finite(entry.timeRemaining, entry.totalTime) ||
                        entry.totalTime <= 0.0f || entry.timeRemaining < 0.0f || entry.timeRemaining > entry.totalTime)
                    {
                        return Fail(error, "invalid production queue entry");
                    }
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
                {
                    return Fail(error, "invalid or duplicate player economy record");
                }
            }
            if (!Finite(snapshot.gatherTimer) || snapshot.gatherTimer < 0.0f ||
                snapshot.gatherTimer >= RTSResourceSystem::GATHER_INTERVAL)
            {
                return Fail(error, "invalid harvest timer");
            }

            std::unordered_set<uint32_t> nodeIds;
            for (const ResourceNode& node : snapshot.resourceNodes)
            {
                if (node.nodeId == 0 || node.nodeId >= snapshot.nextNodeId ||
                    (node.type != RTSResourceType::Minerals && node.type != RTSResourceType::Gas) ||
                    !Finite(node.posX, node.posY) || node.remaining < 0 || node.maxWorkers <= 0 ||
                    node.assignedWorkers.size() > static_cast<size_t>(node.maxWorkers) ||
                    !nodeIds.insert(node.nodeId).second)
                {
                    return Fail(error, "invalid or duplicate resource node");
                }

                // A worker killed this tick stays assigned until the next harvest prunes it, so an id may name a
                // unit that no longer exists; one that does exist must still be a worker.
                std::unordered_set<uint32_t> assigned;
                for (uint32_t workerId : node.assignedWorkers)
                {
                    const auto worker = unitsById.find(workerId);
                    if (workerId == 0 || workerId >= snapshot.nextUnitId || !assigned.insert(workerId).second ||
                        (worker != unitsById.end() && worker->second->type != RTSUnitType::Worker))
                    {
                        return Fail(error, "resource node references an invalid worker");
                    }
                }
            }
            return true;
        }

        bool ValidateOrders(const RTSPersistenceSnapshot& snapshot, std::string& error)
        {
            if (snapshot.commandQueues.size() > MaxRecords || snapshot.selection.size() > MaxRecords)
            {
                return Fail(error, "command record limit exceeded");
            }
            // Queues and selection entries of units killed this tick survive until the next command update.
            for (const auto& [unitId, queue] : snapshot.commandQueues)
            {
                if (unitId == 0 || unitId >= snapshot.nextUnitId || queue.empty() ||
                    queue.size() > RTSCommandSystem::MAX_QUEUED_COMMANDS ||
                    !std::ranges::all_of(queue, RTSCommandSystem::IsCommandValid))
                {
                    return Fail(error, "invalid command queue for unit " + std::to_string(unitId));
                }
            }
            std::unordered_set<uint32_t> selected;
            for (uint32_t unitId : snapshot.selection)
            {
                if (unitId == 0 || unitId >= snapshot.nextUnitId || !selected.insert(unitId).second)
                {
                    return Fail(error, "invalid or duplicate selection entry");
                }
            }
            return true;
        }

        bool ValidateMatchAndFog(const RTSPersistenceSnapshot& snapshot, std::string& error)
        {
            const RTSMatchSnapshot& match = snapshot.match;
            if (match.state >= RTSMatchState::Count || match.winner >= RTSFaction::Count || !Finite(match.matchTime) ||
                match.matchTime < 0.0f || match.players.size() > static_cast<size_t>(RTSMatchSystem::MAX_PLAYERS))
            {
                return Fail(error, "invalid match state");
            }
            for (const PlayerSetup& player : match.players)
            {
                if (player.faction >= RTSFaction::Count || !Finite(player.startX, player.startY))
                {
                    return Fail(error, "invalid match player");
                }
            }

            if (snapshot.fog.size() != static_cast<size_t>(RTSFaction::Count))
            {
                return Fail(error, "fog of war must hold one grid per faction");
            }
            const int width = snapshot.fog.front().width;
            const int height = snapshot.fog.front().height;
            if (width <= 0 || height <= 0 || width > RTSFogOfWarSystem::MAX_MAP_DIMENSION ||
                height > RTSFogOfWarSystem::MAX_MAP_DIMENSION)
            {
                return Fail(error, "invalid fog of war dimensions");
            }
            for (const FogGrid& grid : snapshot.fog)
            {
                if (grid.width != width || grid.height != height ||
                    grid.cells.size() != static_cast<size_t>(width) * static_cast<size_t>(height) ||
                    !std::ranges::all_of(grid.cells, [](RTSVisibility cell) { return cell < RTSVisibility::Count; }))
                {
                    return Fail(error, "invalid fog of war grid");
                }
            }
            return true;
        }
    } // namespace

    bool RTSPersistence::Validate(const RTSPersistenceSnapshot& snapshot, std::string& error)
    {
        if (snapshot.units.size() > MaxRecords || snapshot.buildings.size() > MaxRecords ||
            snapshot.resourceNodes.size() > MaxRecords || snapshot.players.size() > MaxPlayerEconomies)
        {
            return Fail(error, "snapshot record limit exceeded");
        }
        if (snapshot.nextUnitId == 0 || snapshot.nextBuildingId == 0 || snapshot.nextNodeId == 0)
        {
            return Fail(error, "invalid identifier counter");
        }

        std::unordered_map<uint32_t, const UnitData*> unitsById;
        if (!ValidateUnits(snapshot, unitsById, error) || !ValidateBuildings(snapshot, error) ||
            !ValidateEconomy(snapshot, unitsById, error) || !ValidateOrders(snapshot, error) ||
            !ValidateMatchAndFog(snapshot, error))
        {
            return false;
        }

        error.clear();
        return true;
    }

    bool RTSCommandSystem::IsCommandValid(const UnitCommand& command)
    {
        // The order acceptance rule IssueCommand/QueueCommand apply; defined here, beside the
        // snapshot checks that share it, so Validate links without the rest of RTSCommandSystem.
        if (command.type >= RTSCommandType::Count)
        {
            return false;
        }

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
} // namespace RTS
