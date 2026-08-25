/**
 * @file RTSPersistence.h
 * @brief Deterministic, validated snapshot codec for the RTS module's non-ECS state.
 */

#pragma once

#include "Building/RTSBuildingSystem.h"
#include "Command/RTSCommandSystem.h"
#include "Resource/RTSResourceSystem.h"
#include "Unit/RTSUnitSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RTS
{
    struct RTSPersistenceSnapshot
    {
        std::vector<UnitData> units;
        std::vector<BuildingData> buildings;
        std::vector<std::pair<RTSFaction, PlayerResources>> players;
        std::vector<ResourceNode> resourceNodes;
    };

    class RTSPersistence
    {
      public:
        static constexpr std::string_view StateKey = "SparkGameRTS.match.v1";

        static bool IsValidSlotName(std::string_view slotName)
        {
            if (slotName.empty() || slotName.size() > 64)
                return false;
            return std::ranges::all_of(slotName,
                                       [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
        }

        static RTSPersistenceSnapshot Capture(const RTSUnitSystem& units, const RTSBuildingSystem& buildings,
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

            for (const auto& [id, node] : resources.GetNodes())
            {
                (void)id;
                snapshot.resourceNodes.push_back(node);
            }

            std::ranges::sort(snapshot.units, {}, &UnitData::unitId);
            std::ranges::sort(snapshot.buildings, {}, &BuildingData::buildingId);
            std::ranges::sort(snapshot.players, [](const auto& lhs, const auto& rhs)
                              { return static_cast<uint8_t>(lhs.first) < static_cast<uint8_t>(rhs.first); });
            std::ranges::sort(snapshot.resourceNodes, {}, &ResourceNode::nodeId);
            for (ResourceNode& node : snapshot.resourceNodes)
                std::ranges::sort(node.assignedWorkers);
            return snapshot;
        }

        static bool Validate(const RTSPersistenceSnapshot& snapshot, std::string& error)
        {
            constexpr size_t MaxRecords = 10000;
            if (snapshot.units.size() > MaxRecords || snapshot.buildings.size() > MaxRecords ||
                snapshot.resourceNodes.size() > MaxRecords || snapshot.players.size() > 3)
            {
                error = "snapshot record limit exceeded";
                return false;
            }

            std::unordered_map<uint32_t, const UnitData*> unitsById;
            unitsById.reserve(snapshot.units.size());
            for (const UnitData& unit : snapshot.units)
            {
                if (unit.unitId == 0 || unit.unitId == std::numeric_limits<uint32_t>::max())
                {
                    error = "invalid unit identifier";
                    return false;
                }
                if (unit.type >= RTSUnitType::Count || unit.faction >= RTSFaction::Count ||
                    unit.state >= RTSUnitState::Count)
                {
                    error = "invalid unit enum value for id " + std::to_string(unit.unitId);
                    return false;
                }
                if (!Finite(unit.health, unit.maxHealth, unit.damage, unit.attackSpeed, unit.moveSpeed,
                            unit.visionRange, unit.posX, unit.posY))
                {
                    error = "non-finite unit field for id " + std::to_string(unit.unitId);
                    return false;
                }
                if (unit.maxHealth <= 0.0f || unit.health < 0.0f || unit.health > unit.maxHealth ||
                    unit.damage < 0.0f || unit.attackSpeed < 0.0f || unit.moveSpeed < 0.0f || unit.visionRange < 0.0f)
                {
                    error = "invalid unit stat range for id " + std::to_string(unit.unitId);
                    return false;
                }
                if (!unitsById.emplace(unit.unitId, &unit).second)
                {
                    error = "duplicate unit identifier " + std::to_string(unit.unitId);
                    return false;
                }
            }

            std::unordered_set<uint32_t> buildingIds;
            for (const BuildingData& building : snapshot.buildings)
            {
                if (building.buildingId == 0 || building.buildingId == std::numeric_limits<uint32_t>::max() ||
                    building.type >= RTSBuildingType::Count || building.faction >= RTSFaction::Count ||
                    !Finite(building.health, building.maxHealth, building.posX, building.posY,
                            building.constructionProgress, building.constructionTime) ||
                    building.maxHealth <= 0.0f || building.health < 0.0f || building.health > building.maxHealth ||
                    building.constructionProgress < 0.0f || building.constructionProgress > 1.0f ||
                    building.constructionTime <= 0.0f || building.productionQueue.size() > 5 ||
                    !buildingIds.insert(building.buildingId).second)
                {
                    error = "invalid or duplicate building record";
                    return false;
                }
                for (const ProductionEntry& entry : building.productionQueue)
                {
                    if (entry.unitType >= RTSUnitType::Count || !Finite(entry.timeRemaining, entry.totalTime) ||
                        entry.totalTime <= 0.0f || entry.timeRemaining < 0.0f || entry.timeRemaining > entry.totalTime)
                    {
                        error = "invalid production queue entry";
                        return false;
                    }
                }
            }

            std::unordered_set<uint8_t> factions;
            for (const auto& [faction, resources] : snapshot.players)
            {
                const uint8_t value = static_cast<uint8_t>(faction);
                if (faction >= RTSFaction::Count || resources.minerals < 0 || resources.gas < 0 ||
                    resources.currentSupply < 0 || resources.maxSupply < 0 ||
                    resources.currentSupply > resources.maxSupply || !factions.insert(value).second)
                {
                    error = "invalid or duplicate player economy record";
                    return false;
                }
            }

            std::unordered_set<uint32_t> nodeIds;
            for (const ResourceNode& node : snapshot.resourceNodes)
            {
                if (node.nodeId == 0 || node.nodeId == std::numeric_limits<uint32_t>::max() ||
                    (node.type != RTSResourceType::Minerals && node.type != RTSResourceType::Gas) ||
                    !Finite(node.posX, node.posY) || node.remaining < 0 || node.maxWorkers <= 0 ||
                    node.assignedWorkers.size() > static_cast<size_t>(node.maxWorkers) ||
                    !nodeIds.insert(node.nodeId).second)
                {
                    error = "invalid or duplicate resource node";
                    return false;
                }

                std::unordered_set<uint32_t> assigned;
                for (uint32_t workerId : node.assignedWorkers)
                {
                    const auto worker = unitsById.find(workerId);
                    if (worker == unitsById.end() || worker->second->type != RTSUnitType::Worker ||
                        worker->second->state == RTSUnitState::Dead || !assigned.insert(workerId).second)
                    {
                        error = "resource node references an invalid worker";
                        return false;
                    }
                }
            }

            error.clear();
            return true;
        }

        static std::string Serialize(const RTSPersistenceSnapshot& snapshot)
        {
            std::string error;
            return Serialize(snapshot, error);
        }

        static std::string Serialize(const RTSPersistenceSnapshot& snapshot, std::string& error)
        {
            if (!Validate(snapshot, error))
                return {};

            std::ostringstream out;
            out << std::setprecision(std::numeric_limits<float>::max_digits10);
            out << "SPARK_RTS_STATE_V1\nUNITS " << snapshot.units.size() << '\n';
            for (const UnitData& unit : snapshot.units)
            {
                out << "U " << unit.unitId << ' ' << static_cast<unsigned>(unit.type) << ' '
                    << static_cast<unsigned>(unit.faction) << ' ' << static_cast<unsigned>(unit.state) << ' '
                    << unit.health << ' ' << unit.maxHealth << ' ' << unit.damage << ' ' << unit.attackSpeed << ' '
                    << unit.moveSpeed << ' ' << unit.visionRange << ' ' << unit.posX << ' ' << unit.posY << ' '
                    << unit.targetId << '\n';
            }

            out << "BUILDINGS " << snapshot.buildings.size() << '\n';
            for (const BuildingData& building : snapshot.buildings)
            {
                out << "B " << building.buildingId << ' ' << static_cast<unsigned>(building.type) << ' '
                    << static_cast<unsigned>(building.faction) << ' ' << building.health << ' ' << building.maxHealth
                    << ' ' << building.posX << ' ' << building.posY << ' ' << (building.constructionComplete ? 1 : 0)
                    << ' ' << building.constructionProgress << ' ' << building.constructionTime << ' '
                    << building.productionQueue.size() << '\n';
                for (const ProductionEntry& entry : building.productionQueue)
                    out << "P " << static_cast<unsigned>(entry.unitType) << ' ' << entry.timeRemaining << ' '
                        << entry.totalTime << '\n';
            }

            out << "PLAYERS " << snapshot.players.size() << '\n';
            for (const auto& [faction, resources] : snapshot.players)
                out << "R " << static_cast<unsigned>(faction) << ' ' << resources.minerals << ' ' << resources.gas
                    << ' ' << resources.currentSupply << ' ' << resources.maxSupply << '\n';

            out << "NODES " << snapshot.resourceNodes.size() << '\n';
            for (const ResourceNode& node : snapshot.resourceNodes)
            {
                out << "N " << node.nodeId << ' ' << static_cast<unsigned>(node.type) << ' ' << node.posX << ' '
                    << node.posY << ' ' << node.remaining << ' ' << node.maxWorkers << ' '
                    << node.assignedWorkers.size();
                for (uint32_t workerId : node.assignedWorkers)
                    out << ' ' << workerId;
                out << '\n';
            }
            out << "END\n";
            error.clear();
            return out.str();
        }

        static bool Deserialize(std::string_view text, RTSPersistenceSnapshot& outSnapshot, std::string& error)
        {
            constexpr size_t MaxRecords = 10000;
            std::istringstream in{std::string(text)};
            RTSPersistenceSnapshot parsed;
            std::string tag;
            size_t count = 0;

            if (!(in >> tag) || tag != "SPARK_RTS_STATE_V1" || !(in >> tag >> count) || tag != "UNITS" ||
                count > MaxRecords)
                return Fail(error, "invalid RTS snapshot header");
            parsed.units.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                UnitData unit;
                unsigned type = 0, faction = 0, state = 0;
                if (!(in >> tag >> unit.unitId >> type >> faction >> state >> unit.health >> unit.maxHealth >>
                      unit.damage >> unit.attackSpeed >> unit.moveSpeed >> unit.visionRange >> unit.posX >> unit.posY >>
                      unit.targetId) ||
                    tag != "U" || type >= static_cast<unsigned>(RTSUnitType::Count) ||
                    faction >= static_cast<unsigned>(RTSFaction::Count) ||
                    state >= static_cast<unsigned>(RTSUnitState::Count))
                    return Fail(error, "malformed unit record");
                unit.type = static_cast<RTSUnitType>(type);
                unit.faction = static_cast<RTSFaction>(faction);
                unit.state = static_cast<RTSUnitState>(state);
                parsed.units.push_back(unit);
            }

            if (!(in >> tag >> count) || tag != "BUILDINGS" || count > MaxRecords)
                return Fail(error, "malformed building section");
            parsed.buildings.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                BuildingData building;
                unsigned type = 0, faction = 0, complete = 0;
                size_t queueCount = 0;
                if (!(in >> tag >> building.buildingId >> type >> faction >> building.health >> building.maxHealth >>
                      building.posX >> building.posY >> complete >> building.constructionProgress >>
                      building.constructionTime >> queueCount) ||
                    tag != "B" || type >= static_cast<unsigned>(RTSBuildingType::Count) ||
                    faction >= static_cast<unsigned>(RTSFaction::Count) || complete > 1 || queueCount > 5)
                    return Fail(error, "malformed building record");
                building.type = static_cast<RTSBuildingType>(type);
                building.faction = static_cast<RTSFaction>(faction);
                building.constructionComplete = complete != 0;
                building.productionQueue.reserve(queueCount);
                for (size_t queueIndex = 0; queueIndex < queueCount; ++queueIndex)
                {
                    ProductionEntry entry;
                    unsigned unitType = 0;
                    if (!(in >> tag >> unitType >> entry.timeRemaining >> entry.totalTime) || tag != "P" ||
                        unitType >= static_cast<unsigned>(RTSUnitType::Count))
                        return Fail(error, "malformed production queue entry");
                    entry.unitType = static_cast<RTSUnitType>(unitType);
                    building.productionQueue.push_back(entry);
                }
                parsed.buildings.push_back(std::move(building));
            }

            if (!(in >> tag >> count) || tag != "PLAYERS" || count > 3)
                return Fail(error, "malformed player economy section");
            parsed.players.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                unsigned faction = 0;
                PlayerResources resources;
                if (!(in >> tag >> faction >> resources.minerals >> resources.gas >> resources.currentSupply >>
                      resources.maxSupply) ||
                    tag != "R" || faction >= static_cast<unsigned>(RTSFaction::Count))
                    return Fail(error, "malformed player economy record");
                parsed.players.emplace_back(static_cast<RTSFaction>(faction), resources);
            }

            if (!(in >> tag >> count) || tag != "NODES" || count > MaxRecords)
                return Fail(error, "malformed resource node section");
            parsed.resourceNodes.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                ResourceNode node;
                unsigned type = 0;
                size_t workerCount = 0;
                if (!(in >> tag >> node.nodeId >> type >> node.posX >> node.posY >> node.remaining >> node.maxWorkers >>
                      workerCount) ||
                    tag != "N" || type >= static_cast<unsigned>(RTSResourceType::Count) || workerCount > 64)
                    return Fail(error, "malformed resource node record");
                node.type = static_cast<RTSResourceType>(type);
                node.assignedWorkers.reserve(workerCount);
                for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
                {
                    uint32_t workerId = 0;
                    if (!(in >> workerId))
                        return Fail(error, "truncated resource worker list");
                    node.assignedWorkers.push_back(workerId);
                }
                parsed.resourceNodes.push_back(std::move(node));
            }

            if (!(in >> tag) || tag != "END")
                return Fail(error, "missing RTS snapshot terminator");
            in >> std::ws;
            if (!in.eof())
                return Fail(error, "unexpected trailing RTS snapshot data");
            if (!Validate(parsed, error))
                return false;

            outSnapshot = std::move(parsed);
            error.clear();
            return true;
        }

        static bool Apply(const RTSPersistenceSnapshot& snapshot, RTSUnitSystem& units, RTSBuildingSystem& buildings,
                          RTSResourceSystem& resources, RTSCommandSystem* commands, std::string& error)
        {
            if (!Validate(snapshot, error))
                return false;

            const RTSPersistenceSnapshot previous = Capture(units, buildings, resources);
            if (!units.RestoreState(snapshot.units) ||
                !resources.RestoreState(snapshot.players, snapshot.resourceNodes) ||
                !buildings.RestoreState(snapshot.buildings))
            {
                units.RestoreState(previous.units);
                resources.RestoreState(previous.players, previous.resourceNodes);
                buildings.RestoreState(previous.buildings);
                return Fail(error, "RTS systems rejected a validated snapshot");
            }

            if (commands)
                commands->ResetRuntimeState();
            error.clear();
            return true;
        }

      private:
        template <typename... Values> static bool Finite(Values... values)
        {
            return (... && std::isfinite(static_cast<double>(values)));
        }

        static bool Fail(std::string& error, std::string message)
        {
            error = std::move(message);
            return false;
        }
    };
} // namespace RTS
