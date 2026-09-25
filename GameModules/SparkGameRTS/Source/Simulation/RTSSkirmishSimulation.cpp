/**
 * @file RTSSkirmishSimulation.cpp
 * @brief Deterministic fixed-step skirmish tick: commands, combat, economy, fog, AI, and win/loss
 */

#include "RTSSkirmishSimulation.h"

#include "Building/RTSBuildingSystem.h"
#include "Command/RTSCommandSystem.h"
#include "FogOfWar/RTSFogOfWarSystem.h"
#include "Match/RTSMatchSystem.h"
#include "Resource/RTSResourceSystem.h"
#include "Unit/RTSUnitSystem.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace RTS
{
    namespace
    {
        /// Buildings are hit from anywhere on their footprint, not only their centre point.
        constexpr float BUILDING_RADIUS = 2.0f;

        constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
        constexpr uint64_t FNV_PRIME = 1099511628211ull;

        struct StateHasher
        {
            uint64_t value = FNV_OFFSET;

            void Bytes(uint64_t data, int byteCount)
            {
                for (int i = 0; i < byteCount; ++i)
                {
                    value ^= (data >> (i * 8)) & 0xFFu;
                    value *= FNV_PRIME;
                }
            }
            void U8(uint8_t data) { Bytes(data, 1); }
            void U32(uint32_t data) { Bytes(data, 4); }
            void U64(uint64_t data) { Bytes(data, 8); }
            void I32(int data) { Bytes(static_cast<uint32_t>(data), 4); }
            void F32(float data) { Bytes(std::bit_cast<uint32_t>(data), 4); }
        };

        /// Squared distance written as separate, correctly rounded operations so no compiler may contract it.
        float DistanceSquared(float ax, float ay, float bx, float by)
        {
            const float dx = bx - ax;
            const float dy = by - ay;
            const float dx2 = dx * dx;
            const float dy2 = dy * dy;
            return dx2 + dy2;
        }

        /// Every live unit id across all factions in ascending order: the canonical simulation order.
        std::vector<uint32_t> AllUnitIds(const RTSUnitSystem& units)
        {
            std::vector<uint32_t> ids;
            ids.reserve(units.GetUnitCount());
            for (int faction = 0; faction < static_cast<int>(RTSFaction::Count); ++faction)
            {
                for (uint32_t id : units.GetUnitsByFaction(static_cast<RTSFaction>(faction)))
                    ids.push_back(id);
            }
            std::ranges::sort(ids);
            return ids;
        }

        std::vector<uint32_t> AllBuildingIds(const RTSBuildingSystem& buildings)
        {
            std::vector<uint32_t> ids;
            ids.reserve(buildings.GetBuildingCount());
            for (int faction = 0; faction < static_cast<int>(RTSFaction::Count); ++faction)
            {
                for (uint32_t id : buildings.GetBuildingsByFaction(static_cast<RTSFaction>(faction)))
                    ids.push_back(id);
            }
            std::ranges::sort(ids);
            return ids;
        }

        struct PendingHit
        {
            bool isBuilding = false;
            uint32_t targetId = 0;
            float amount = 0.0f;
        };
        /// Nearest living enemy unit in attack range, else the nearest enemy structure; ties go to the lowest id.
        PendingHit FindTarget(const UnitData& attacker, const RTSUnitSystem& units, const RTSBuildingSystem& buildings,
                              const std::vector<uint32_t>& unitIds, const std::vector<uint32_t>& buildingIds)
        {
            const UnitTemplate* unitTemplate = units.GetTemplate(attacker.type, attacker.faction);
            const float range = unitTemplate ? unitTemplate->attackRange : 1.0f;
            const float buildingReach = range + BUILDING_RADIUS;

            PendingHit hit;
            float bestDistanceSquared = range * range;
            for (uint32_t targetId : unitIds)
            {
                const UnitData* target = units.GetUnit(targetId);
                if (!target || target->faction == attacker.faction || target->state == RTSUnitState::Dead)
                    continue;
                const float distanceSquared = DistanceSquared(attacker.posX, attacker.posY, target->posX, target->posY);
                // Strictly closer replaces; the first (lowest-id) candidate wins an exact tie.
                if (hit.targetId == 0 ? distanceSquared <= bestDistanceSquared : distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    hit.targetId = targetId;
                }
            }
            if (hit.targetId != 0)
                return hit;

            bestDistanceSquared = buildingReach * buildingReach;
            for (uint32_t targetId : buildingIds)
            {
                const BuildingData* target = buildings.GetBuilding(targetId);
                if (!target || target->faction == attacker.faction || target->health <= 0.0f)
                    continue;
                const float distanceSquared = DistanceSquared(attacker.posX, attacker.posY, target->posX, target->posY);
                if (hit.targetId == 0 ? distanceSquared <= bestDistanceSquared : distanceSquared < bestDistanceSquared)
                {
                    bestDistanceSquared = distanceSquared;
                    hit.targetId = targetId;
                    hit.isBuilding = true;
                }
            }
            return hit;
        }
    } // namespace

    bool RTSSkirmishSimulation::Initialize(Spark::IEngineContext* context, const RTSSkirmishSystems& systems)
    {
        if (!systems.units || !systems.buildings || !systems.resources || !systems.commands || !systems.fog ||
            !systems.match)
        {
            return false;
        }
        m_context = context;
        m_systems = systems;
        ResetClock();
        return true;
    }

    void RTSSkirmishSimulation::Shutdown()
    {
        m_systems = {};
        m_context = nullptr;
        ResetClock();
    }

    bool RTSSkirmishSimulation::StartDefaultSkirmish()
    {
        auto& [units, buildings, resources, commands, fog, match] = m_systems;
        if (!units)
            return false;

        commands->Shutdown();
        buildings->Shutdown();
        resources->Shutdown();
        units->Shutdown();
        fog->Shutdown();
        match->Shutdown();

        if (!units->Initialize(m_context) || !resources->Initialize(m_context, units) ||
            !buildings->Initialize(m_context, units, resources) || !commands->Initialize(m_context, units) ||
            !fog->Initialize(m_context, MAP_SIZE, MAP_SIZE) || !match->Initialize(m_context))
        {
            return false;
        }

        resources->InitializePlayer(RTSFaction::Human);
        resources->InitializePlayer(RTSFaction::Swarm);
        match->SetupMatch(2);
        match->SetPlayerFaction(0, RTSFaction::Human);
        match->SetPlayerStartPosition(0, 18.0f, 22.0f);
        match->SetPlayerFaction(1, RTSFaction::Swarm);
        match->SetPlayerStartPosition(1, 78.0f, 74.0f);
        match->SetPlayerIsAI(1, true);

        const auto spawnStartingUnit = [units, resources](RTSUnitType type, RTSFaction faction, float x, float y)
        {
            const uint32_t unitId = units->SpawnUnit(type, faction, x, y);
            if (unitId != 0)
            {
                if (const UnitTemplate* unitTemplate = units->GetTemplate(type, faction))
                    resources->UseSupply(faction, unitTemplate->cost.supply);
            }
            return unitId;
        };

        const uint32_t humanWorker = spawnStartingUnit(RTSUnitType::Worker, RTSFaction::Human, 21.0f, 24.0f);
        spawnStartingUnit(RTSUnitType::Marine, RTSFaction::Human, 26.0f, 25.0f);
        spawnStartingUnit(RTSUnitType::Marine, RTSFaction::Human, 29.0f, 27.0f);
        spawnStartingUnit(RTSUnitType::Marine, RTSFaction::Human, 25.0f, 29.0f);
        spawnStartingUnit(RTSUnitType::Tank, RTSFaction::Human, 21.0f, 31.0f);

        const uint32_t swarmWorker = spawnStartingUnit(RTSUnitType::Worker, RTSFaction::Swarm, 76.0f, 72.0f);
        spawnStartingUnit(RTSUnitType::Marine, RTSFaction::Swarm, 69.0f, 70.0f);
        spawnStartingUnit(RTSUnitType::Marine, RTSFaction::Swarm, 72.0f, 67.0f);
        spawnStartingUnit(RTSUnitType::Tank, RTSFaction::Swarm, 76.0f, 65.0f);

        buildings->PlaceBuilding(RTSBuildingType::CommandCenter, RTSFaction::Human, 16.0f, 18.0f);
        buildings->PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Human, 31.0f, 19.0f);
        buildings->PlaceBuilding(RTSBuildingType::CommandCenter, RTSFaction::Swarm, 80.0f, 78.0f);
        buildings->PlaceBuilding(RTSBuildingType::Barracks, RTSFaction::Swarm, 66.0f, 78.0f);
        buildings->Update(120.0f); // Starting bases are already built

        const uint32_t humanMinerals = resources->CreateNode(RTSResourceType::Minerals, 13.0f, 29.0f, 1500);
        resources->CreateNode(RTSResourceType::Gas, 36.0f, 15.0f, 900);
        const uint32_t swarmMinerals = resources->CreateNode(RTSResourceType::Minerals, 82.0f, 67.0f, 1500);
        resources->CreateNode(RTSResourceType::Gas, 61.0f, 82.0f, 900);
        resources->AssignWorker(humanMinerals, humanWorker);
        resources->AssignWorker(swarmMinerals, swarmWorker);

        match->StartMatch();
        ResetClock();
        RefreshVision();
        return true;
    }

    uint32_t RTSSkirmishSimulation::Advance(float frameSeconds)
    {
        if (!std::isfinite(frameSeconds) || frameSeconds <= 0.0f)
            return 0;

        m_accumulatedSeconds += frameSeconds;
        uint32_t ticks = 0;
        while (m_accumulatedSeconds >= TICK_SECONDS && ticks < MAX_TICKS_PER_ADVANCE)
        {
            Step();
            m_accumulatedSeconds -= TICK_SECONDS;
            ++ticks;
        }
        // A long hitch drops the remaining backlog rather than stalling future frames.
        if (ticks == MAX_TICKS_PER_ADVANCE && m_accumulatedSeconds >= TICK_SECONDS)
            m_accumulatedSeconds = 0.0;
        return ticks;
    }

    void RTSSkirmishSimulation::Step()
    {
        auto& [units, buildings, resources, commands, fog, match] = m_systems;
        if (!units || match->GetMatchState() != RTSMatchState::Playing)
            return;

        if (m_tick % AI_DECISION_TICKS == 0)
            RunAIOpponents();
        commands->Update(TICK_SECONDS);
        ResolveCombat();
        units->Update(TICK_SECONDS);
        buildings->Update(TICK_SECONDS);
        resources->Update(TICK_SECONDS);
        fog->Update(TICK_SECONDS);
        RefreshVision();
        UpdateEliminations();
        match->Update(TICK_SECONDS);
        ++m_tick;
    }

    void RTSSkirmishSimulation::ResetClock()
    {
        m_accumulatedSeconds = 0.0;
        m_tick = 0;
    }

    void RTSSkirmishSimulation::RestoreClock(uint64_t tick)
    {
        m_accumulatedSeconds = 0.0;
        m_tick = tick;
    }

    uint64_t RTSSkirmishSimulation::GetTick() const
    {
        return m_tick;
    }

    void RTSSkirmishSimulation::RunAIOpponents()
    {
        auto& [units, buildings, resources, commands, fog, match] = m_systems;
        for (int playerIndex = 0; playerIndex < match->GetPlayerCount(); ++playerIndex)
        {
            const PlayerSetup* player = match->GetPlayer(playerIndex);
            if (!player || !player->isAI || player->isEliminated)
                continue;
            const RTSFaction faction = player->faction;

            // Keep every idle barracks producing; StartProduction enforces cost and supply.
            for (uint32_t buildingId : buildings->GetBuildingsByFaction(faction))
            {
                const BuildingData* building = buildings->GetBuilding(buildingId);
                if (building && building->type == RTSBuildingType::Barracks && building->constructionComplete &&
                    building->productionQueue.empty())
                {
                    buildings->StartProduction(buildingId, RTSUnitType::Marine);
                }
            }

            // Attack the first surviving enemy's oldest structure (or unit) once the idle army is large enough.
            std::vector<uint32_t> idleArmy;
            for (uint32_t unitId : units->GetUnitsByFaction(faction))
            {
                const UnitData* unit = units->GetUnit(unitId);
                if (unit && unit->type != RTSUnitType::Worker && !commands->GetCurrentCommand(unitId) &&
                    (unit->state == RTSUnitState::Idle || unit->state == RTSUnitState::Holding))
                {
                    idleArmy.push_back(unitId);
                }
            }
            if (static_cast<int>(idleArmy.size()) < AI_ATTACK_WAVE_SIZE)
                continue;

            for (int enemyIndex = 0; enemyIndex < match->GetPlayerCount(); ++enemyIndex)
            {
                const PlayerSetup* enemy = match->GetPlayer(enemyIndex);
                if (!enemy || enemy->isEliminated || enemy->faction == faction)
                    continue;

                float targetX = 0.0f;
                float targetY = 0.0f;
                const std::vector<uint32_t> enemyBuildings = buildings->GetBuildingsByFaction(enemy->faction);
                const std::vector<uint32_t> enemyUnits = units->GetUnitsByFaction(enemy->faction);
                if (!enemyBuildings.empty())
                {
                    const BuildingData* target = buildings->GetBuilding(enemyBuildings.front());
                    targetX = target->posX;
                    targetY = target->posY;
                }
                else if (!enemyUnits.empty())
                {
                    const UnitData* target = units->GetUnit(enemyUnits.front());
                    targetX = target->posX;
                    targetY = target->posY;
                }
                else
                {
                    continue;
                }

                for (uint32_t unitId : idleArmy)
                    commands->IssueCommand(unitId, {RTSCommandType::Attack, targetX, targetY, 0});
                break;
            }
        }
    }

    void RTSSkirmishSimulation::ResolveCombat()
    {
        auto& [units, buildings, resources, commands, fog, match] = m_systems;
        const std::vector<uint32_t> unitIds = AllUnitIds(*units);
        const std::vector<uint32_t> buildingIds = AllBuildingIds(*buildings);

        // Every attacker picks its target from the same start-of-tick state; hits land together afterwards so
        // the outcome does not depend on which attacker is evaluated first.
        std::vector<PendingHit> hits;
        for (uint32_t attackerId : unitIds)
        {
            UnitData* attacker = units->GetUnitMutable(attackerId);
            if (!attacker || attacker->damage <= 0.0f || attacker->attackSpeed <= 0.0f ||
                attacker->state == RTSUnitState::Dead || attacker->state == RTSUnitState::Moving ||
                attacker->state == RTSUnitState::Gathering || attacker->state == RTSUnitState::Building)
            {
                continue;
            }

            PendingHit hit = FindTarget(*attacker, *units, *buildings, unitIds, buildingIds);
            if (hit.targetId != 0)
            {
                const float perSecond = attacker->damage * attacker->attackSpeed;
                hit.amount = perSecond * TICK_SECONDS;
                hits.push_back(hit);
                attacker->targetId = hit.targetId;
                if (attacker->state != RTSUnitState::Holding)
                    attacker->state = RTSUnitState::Attacking;
            }
            else if (attacker->state == RTSUnitState::Attacking)
            {
                // Nothing in reach: an attack-move keeps walking, a finished engagement goes idle.
                attacker->targetId = 0;
                if (!commands->GetCurrentCommand(attackerId))
                    attacker->state = RTSUnitState::Idle;
            }
        }

        for (const PendingHit& hit : hits)
        {
            if (hit.isBuilding)
            {
                buildings->ApplyDamage(hit.targetId, hit.amount);
                continue;
            }
            UnitData* target = units->GetUnitMutable(hit.targetId);
            if (!target || target->state == RTSUnitState::Dead)
                continue;
            target->health -= hit.amount;
            if (target->health <= 0.0f)
            {
                target->health = 0.0f;
                target->state = RTSUnitState::Dead;
                target->targetId = 0;
                if (const UnitTemplate* unitTemplate = units->GetTemplate(target->type, target->faction))
                    resources->FreeSupply(target->faction, unitTemplate->cost.supply);
            }
        }
    }

    void RTSSkirmishSimulation::RefreshVision()
    {
        auto& [units, buildings, resources, commands, fog, match] = m_systems;
        for (int factionIndex = 0; factionIndex < static_cast<int>(RTSFaction::Count); ++factionIndex)
        {
            const auto faction = static_cast<RTSFaction>(factionIndex);
            fog->ClearCurrentVision(faction);
            for (uint32_t unitId : units->GetUnitsByFaction(faction))
            {
                if (const UnitData* unit = units->GetUnit(unitId))
                    fog->UpdateVision(faction, unit->posX, unit->posY, unit->visionRange);
            }
        }
    }

    void RTSSkirmishSimulation::UpdateEliminations()
    {
        auto& [units, buildings, resources, commands, fog, match] = m_systems;
        for (int playerIndex = 0; playerIndex < match->GetPlayerCount(); ++playerIndex)
        {
            const PlayerSetup* player = match->GetPlayer(playerIndex);
            if (player && !player->isEliminated && units->GetUnitCountByFaction(player->faction) == 0 &&
                buildings->GetBuildingCountByFaction(player->faction) == 0)
            {
                match->MarkPlayerEliminated(playerIndex);
            }
        }
    }

    uint64_t RTSSkirmishSimulation::ComputeStateHash() const
    {
        const auto& [units, buildings, resources, commands, fog, match] = m_systems;
        StateHasher hash;
        hash.U64(m_tick);
        if (!units)
            return hash.value;

        hash.U8(static_cast<uint8_t>(match->GetMatchState()));
        hash.F32(match->GetMatchTime());
        hash.U8(static_cast<uint8_t>(match->HasWinner()));
        hash.U8(static_cast<uint8_t>(match->GetWinner()));
        hash.I32(match->GetPlayerCount());
        for (int playerIndex = 0; playerIndex < match->GetPlayerCount(); ++playerIndex)
        {
            const PlayerSetup* player = match->GetPlayer(playerIndex);
            hash.U8(static_cast<uint8_t>(player->faction));
            hash.F32(player->startX);
            hash.F32(player->startY);
            hash.U8(static_cast<uint8_t>(player->isAI));
            hash.U8(static_cast<uint8_t>(player->hasSurrendered));
            hash.U8(static_cast<uint8_t>(player->isEliminated));
        }

        hash.U32(units->GetNextUnitId());
        for (uint32_t unitId : AllUnitIds(*units))
        {
            const UnitData* unit = units->GetUnit(unitId);
            hash.U32(unit->unitId);
            hash.U8(static_cast<uint8_t>(unit->type));
            hash.U8(static_cast<uint8_t>(unit->faction));
            hash.U8(static_cast<uint8_t>(unit->state));
            hash.F32(unit->health);
            hash.F32(unit->maxHealth);
            hash.F32(unit->damage);
            hash.F32(unit->attackSpeed);
            hash.F32(unit->moveSpeed);
            hash.F32(unit->visionRange);
            hash.F32(unit->posX);
            hash.F32(unit->posY);
            hash.U32(unit->targetId);
        }

        // Every queued order, including queues of units killed this tick that the next Update prunes.
        hash.U64(commands->GetCommandQueues().size());
        for (const auto& [unitId, queue] : commands->GetCommandQueues())
        {
            hash.U32(unitId);
            hash.U64(queue.size());
            for (const UnitCommand& command : queue)
            {
                hash.U8(static_cast<uint8_t>(command.type));
                hash.F32(command.targetX);
                hash.F32(command.targetY);
                hash.U32(command.targetEntity);
            }
        }
        hash.U64(commands->GetSelection().size());
        for (uint32_t unitId : commands->GetSelection())
            hash.U32(unitId);

        hash.U32(buildings->GetNextBuildingId());
        for (uint32_t buildingId : AllBuildingIds(*buildings))
        {
            const BuildingData* building = buildings->GetBuilding(buildingId);
            hash.U32(building->buildingId);
            hash.U8(static_cast<uint8_t>(building->type));
            hash.U8(static_cast<uint8_t>(building->faction));
            hash.F32(building->health);
            hash.F32(building->maxHealth);
            hash.F32(building->posX);
            hash.F32(building->posY);
            hash.U8(static_cast<uint8_t>(building->constructionComplete));
            hash.F32(building->constructionProgress);
            hash.F32(building->constructionTime);
            hash.U64(building->productionQueue.size());
            for (const ProductionEntry& entry : building->productionQueue)
            {
                hash.U8(static_cast<uint8_t>(entry.unitType));
                hash.F32(entry.timeRemaining);
                hash.F32(entry.totalTime);
            }
        }

        for (int factionIndex = 0; factionIndex < static_cast<int>(RTSFaction::Count); ++factionIndex)
        {
            const auto faction = static_cast<RTSFaction>(factionIndex);
            if (const PlayerResources* player = resources->GetPlayerResources(faction))
            {
                hash.U8(static_cast<uint8_t>(faction));
                hash.I32(player->minerals);
                hash.I32(player->gas);
                hash.I32(player->currentSupply);
                hash.I32(player->maxSupply);
            }
            if (const FogGrid* grid = fog->GetGrid(faction))
            {
                hash.I32(grid->width);
                hash.I32(grid->height);
                for (RTSVisibility cell : grid->cells)
                    hash.U8(static_cast<uint8_t>(cell));
            }
        }

        hash.U32(resources->GetNextNodeId());
        hash.F32(resources->GetGatherTimer());
        for (const auto& [nodeId, node] : resources->GetNodes())
        {
            hash.U32(nodeId);
            hash.U8(static_cast<uint8_t>(node.type));
            hash.F32(node.posX);
            hash.F32(node.posY);
            hash.I32(node.remaining);
            hash.I32(node.maxWorkers);
            hash.U64(node.assignedWorkers.size());
            for (uint32_t workerId : node.assignedWorkers)
                hash.U32(workerId);
        }
        return hash.value;
    }

} // namespace RTS
