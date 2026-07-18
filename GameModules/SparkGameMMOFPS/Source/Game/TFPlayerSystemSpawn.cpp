/**
 * @file TFPlayerSystemSpawn.cpp
 * @brief TFPlayerSystem spawn-request flow: TF_SpawnRequest validation
 *        (faction / class / timer / spawn kind), spawn-point selection
 *        (skyanchor, region, Aegis, squad leader) and the TF_SpawnReply
 *        send path. Pawn entity creation and the kill/respawn lifecycle
 *        live in TFPlayerSystem.cpp (same class, split per repo file-size
 *        rules).
 */
#include "Game/TFPlayerSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFVehicleSystem.h" // W3 shared-edit: Aegis mobile-spawn (spawnKind==2)
#include "Game/TFSquadSystem.h"   // W4: squad-leader spawn (spawnKind==3)
#include "World/TFRegionSystem.h" // W4: region spawn (spawnKind==1)
#include "Net/TFServerSim.h"
#include "UI/TFHUD.h"
#include "World/TFWorldSetup.h"

#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace Terrafront
{

    namespace
    {

        constexpr float kSpawnLiftM = 0.10f;  // spawn epsilon above the terrain
        constexpr float kMapCenter = 2048.0f; // face the middle of Cindral Wastes

    } // namespace

    void TFPlayerSystem::ServerHandleSpawnRequest(PlayerId player, const TF_SpawnRequest& req)
    {
        PlayerRec& rec = m_players[player];

        // Faction can arrive via ServerHandleFactionSelect or TFServerSim's
        // registry (network FactionSelect routing) — accept either.
        FactionId faction = rec.faction;
        if (faction == FactionId::None && m_ctx->serverSim)
            faction = m_ctx->serverSim->GetPlayerFaction(player);

        TF_SpawnReply reply{};
        reply.accepted = 0;
        float pos[3] = {0, 0, 0};
        float yaw = 0.0f;

        if (faction == FactionId::None)
        {
            reply.reason = 1; // must pick a faction first
        }
        else if (req.classId >= static_cast<uint8_t>(ClassId::COUNT) ||
                 req.classId == static_cast<uint8_t>(ClassId::Colossus))
        {
            reply.reason = 4; // class-locked (Colossus is terminal-purchased)
        }
        // W3 shared-edit (vehicles agent): spawnKind==2 (deployed friendly Aegis)
        // is now a first-class spawn point. Its respawn timer is shorter (DESIGN
        // §4: 5 s at an Aegis vs the 8 s default, data-driven via
        // vehicles.json deployRespawnSec).
        else if (req.spawnKind > 3)
        {
            reply.reason = 1; // unknown spawn kind
        }
        else if (req.spawnKind == 1 && !m_ctx->regions)
        {
            reply.reason = 1; // region system absent (headless unit tests)
        }
        else if (req.spawnKind == 3 && !m_ctx->squads)
        {
            reply.reason = 1; // squad system absent
        }
        else if (req.spawnKind == 2 && !m_ctx->vehicles)
        {
            reply.reason = 1; // vehicles system absent (headless unit tests)
        }
        else if (rec.alive)
        {
            reply.reason = 1; // already deployed
        }
        else if (const double respawnAt =
                     rec.nextRespawnAt -
                     ((req.spawnKind == 2 && m_ctx->vehicles)
                          ? std::max(0.0f, kTFRespawnDelaySec - m_ctx->vehicles->AegisRespawnDelaySec())
                          : 0.0f);
                 NowSec() < respawnAt)
        {
            reply.reason = 2; // respawn timer
            reply.respawnDelay = static_cast<float>(respawnAt - NowSec());
        }
        else if (![&]
                 {
                     switch (req.spawnKind)
                     {
                     case 1:
                         return FindRegionSpawn(req.regionId, faction, pos, yaw);
                     case 2:
                         return m_ctx->vehicles->GetAegisSpawnPos(req.aegisEntity, faction, pos);
                     case 3:
                         return m_ctx->squads->GetSquadLeaderSpawn(player, pos);
                     default:
                         return FindSkyanchorSpawn(faction, pos, yaw);
                     }
                 }())
        {
            reply.reason = req.spawnKind == 2 ? 3 : 1; // 3 = aegis gone/undeployed/contested
            if (req.spawnKind == 3)
            {
                const float cd = m_ctx->squads->SquadSpawnCooldownRemaining(player);
                if (cd > 0.0f)
                {
                    reply.reason = 2; // squad-spawn cooldown
                    reply.respawnDelay = cd;
                }
            }
        }
        else
        {
            rec.faction = faction;
            const EntityId pawn = ServerSpawnPawn(player, faction, static_cast<ClassId>(req.classId), pos, yaw);
            if (pawn != 0)
            {
                reply.accepted = 1;
                reply.reason = 0;
                reply.entityId = pawn;
                reply.posX = pos[0];
                reply.posY = pos[1];
                reply.posZ = pos[2];
            }
        }
        if (!reply.accepted && reply.reason != 2) // reason 2 = respawn timer (normal, retried)
            Spark::SimpleConsole::GetInstance().LogWarning("[TF] spawn DENIED p" + std::to_string(player) + " reason " +
                                                           std::to_string(reply.reason));
        SendSpawnReply(player, reply);
    }

    void TFPlayerSystem::SendSpawnReply(PlayerId player, const TF_SpawnReply& reply)
    {
        // In-process local player (listen host / standalone): no socket round-trip.
        if (m_ctx && m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer)
        {
            ClientOnSpawnReply(reply); // no-op on the authority by contract
            if (m_ctx->hud)
            {
                if (reply.accepted)
                    m_ctx->hud->SetRespawnState(false, 0.0f);
                else if (reply.reason == 2)
                    m_ctx->hud->SetRespawnState(true, reply.respawnDelay);
            }
            return;
        }

#ifdef ENABLE_NETWORKING
        if (m_ctx && m_ctx->role != NetRole::Standalone)
        {
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.IsInitialized())
            {
                Spark::Net::NetworkMessage msg;
                msg.type = static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(TFMsg::SpawnReply));
                msg.channel = Spark::Net::ChannelType::Reliable;
                msg.payload.resize(sizeof(reply));
                std::memcpy(msg.payload.data(), &reply, sizeof(reply));
                nm.SendToClient(player, msg);
            }
        }
#endif
    }

    // W4 integration: spawn at an owned, lattice-connected region (map-screen
    // click deploys send spawnKind==1). Point comes from regions.json spawns.
    bool TFPlayerSystem::FindRegionSpawn(RegionId region, FactionId faction, float outPos[3], float& outYaw) const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded() || !m_ctx->regions)
            return false;
        if (!m_ctx->regions->CanSpawnAt(region, faction))
            return false;
        const RegionDef* r = m_ctx->data->GetRegion(region);
        if (!r)
            return false;
        const float x = r->spawns.empty() ? r->centerX : r->spawns.front()[0];
        const float z = r->spawns.empty() ? r->centerZ : r->spawns.front()[1];
        outPos[0] = x;
        outPos[1] = m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) + 0.1f : 0.1f;
        outPos[2] = z;
        outYaw = std::atan2(2048.0f - x, 2048.0f - z); // face map center
        return true;
    }

    bool TFPlayerSystem::FindSkyanchorSpawn(FactionId faction, float outPos[3], float& outYaw) const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        for (const auto& r : m_ctx->data->GetContinent().regions)
        {
            if (r.tier == "skyanchor" && r.homeFaction == faction)
            {
                const float x = r.spawns.empty() ? r.centerX : r.spawns.front()[0];
                const float z = r.spawns.empty() ? r.centerZ : r.spawns.front()[1];
                outPos[0] = x;
                outPos[1] = (m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f) + kSpawnLiftM;
                outPos[2] = z;
                outYaw = std::atan2(kMapCenter - x, kMapCenter - z); // face map center
                return true;
            }
        }
        return false;
    }

} // namespace Terrafront
