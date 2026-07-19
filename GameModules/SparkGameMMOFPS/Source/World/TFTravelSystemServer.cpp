/**
 * @file TFTravelSystemServer.cpp
 * @brief TFTravelSystem server half: sanctuary first-spawn placement, stale
 *        mapId pruning, warpgate/pad destination lookup, travel-request
 *        validation and the continent info summary. Split from
 *        TFTravelSystem.cpp (same class, split per the repo file-size rules —
 *        mirrors the TFVehicleSystem split); shared internals live in
 *        TFTravelSystemInternal.h.
 */
#include "World/TFTravelSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFServerSim.h"
#include "World/TFRegionSystem.h"
#include "World/TFTravelSystemInternal.h"
#include "World/TFWorldSetup.h"

#include "Utils/LogMacros.h"

namespace Terrafront
{

    using namespace TravelDetail;

    namespace
    {

        constexpr float kServerUseSlackM = 2.0f; ///< latency slack on the server-side terminal check
        constexpr float kPruneEverySec = 10.0f;  ///< stale mapId sweep cadence

    } // namespace

    void TFTravelSystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->serverSim)
            return;

        // Only sessions that passed the enter-world gate get sanctuary placement.
        // Bots (kTFBotIdBase ids) and pre-gate connections are never in
        // m_enteredWorld, so they spawn wherever the spawn pipeline put them.
        // (Builds without ENABLE_NETWORKING never populate the gate — the
        // sanctuary flow simply stays dormant there.)
        if (!m_ctx->serverSim->IsEnteredWorld(ev.player))
            return;

        const auto it = m_mapOf.try_emplace(ev.player, kTFMapSanctuary).first;
        if (it->second != kTFMapSanctuary)
            return; // continent resident: normal skyanchor/region spawn stands

        // DO NOT teleport here: this event fires synchronously inside the spawn
        // handlers, which write MoveState AFTER firing it (TFServerSim
        // HandleSpawnRequest seeds m_move[sender] post-ServerSpawnPawn). Queue
        // for the next authoritative fixed tick instead.
        m_pendingPlace.insert(ev.player);
    }

    void TFTravelSystem::ServerPlacePending()
    {
        if (m_pendingPlace.empty() || !m_ctx->serverSim)
            return;
        for (const PlayerId pid : m_pendingPlace)
        {
            if (!m_ctx->serverSim->IsPlayerAlive(pid))
                continue; // died/despawned between event and tick
            float pos[3];
            ServerSanctuaryPadOf(pid, pos);
            m_ctx->serverSim->TeleportPawn(pid, pos[0], pos[1], pos[2]);
            ++m_placements;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u placed in %s (%.0f, %.0f)", pid,
                           m_sanctuaryDisplayName.c_str(), pos[0], pos[2]);
        }
        m_pendingPlace.clear();
    }

    void TFTravelSystem::ServerPruneStale(float dt)
    {
        m_pruneAccum += dt;
        if (m_pruneAccum < kPruneEverySec || !m_ctx->serverSim)
            return;
        m_pruneAccum = 0.0f;
        for (auto it = m_mapOf.begin(); it != m_mapOf.end();)
        {
            if (!m_ctx->serverSim->IsEnteredWorld(it->first) && !m_ctx->serverSim->IsPlayerAlive(it->first))
                it = m_mapOf.erase(it);
            else
                ++it;
        }
    }

    void TFTravelSystem::ServerSanctuaryPadOf(PlayerId player, float outPos[3]) const
    {
        const uint32_t pad = player % kTFSanctuarySpawnPadCount;
        outPos[0] = kTFSanctuarySpawnPads[pad][0];
        outPos[2] = kTFSanctuarySpawnPads[pad][1];
        outPos[1] = m_ctx->world ? m_ctx->world->TerrainHeightAt(outPos[0], outPos[2]) : kTFSanctuaryPadY;
    }

    bool TFTravelSystem::ServerWarpgateOf(FactionId faction, float outPos[3]) const
    {
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        for (const RegionDef& r : m_ctx->data->GetContinent().regions)
        {
            if (r.tier != "skyanchor" || r.homeFaction != faction)
                continue;
            outPos[0] = r.spawns.empty() ? r.centerX : (*r.spawns.begin())[0];
            outPos[2] = r.spawns.empty() ? r.centerZ : (*r.spawns.begin())[1];
            outPos[1] = m_ctx->world ? m_ctx->world->TerrainHeightAt(outPos[0], outPos[2]) : 0.0f;
            return true;
        }
        return false;
    }

    void TFTravelSystem::ServerHandleTravel(PlayerId sender, const TF_TravelRequest& req)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->serverSim)
            return;

        TF_TravelReply rep{};
        rep.accepted = 0;
        rep.mapId = req.mapId;

        PawnInfo pawn{};
        const bool havePawn = m_ctx->players && m_ctx->players->GetPawnByPlayer(sender, pawn) && pawn.alive;

        if (req.mapId >= kTFMapCount)
            rep.reason = static_cast<uint8_t>(TFTravelErr::BadMap);
#ifdef ENABLE_NETWORKING
        // Same enter-world gate every client-originated gameplay message is
        // held to (TFServerSim::RouteClientMessage choke point / the vehicle
        // authority-path mirror). Gate exists only with networking built in.
        else if (!m_ctx->serverSim->IsEnteredWorld(sender))
            rep.reason = static_cast<uint8_t>(TFTravelErr::NotEntered);
#endif
        else if (!m_ctx->serverSim->IsPlayerAlive(sender) || !havePawn)
            rep.reason = static_cast<uint8_t>(TFTravelErr::NotAlive);
        else
        {
            const uint8_t cur = TFTravel_MapIdAt(pawn.pos[0], pawn.pos[2]);
            if (cur == req.mapId)
                rep.reason = static_cast<uint8_t>(TFTravelErr::AlreadyThere);
            else if (cur == kTFMapSanctuary &&
                     DistSqXZ(pawn.pos, kTFSanctuaryTerminalX, kTFSanctuaryTerminalZ) >
                         (kTFTravelTerminalUseM + kServerUseSlackM) * (kTFTravelTerminalUseM + kServerUseSlackM))
            {
                // Leaving the sanctuary requires standing at the terminal.
                // (Continent -> sanctuary is an unrestricted recall.)
                rep.reason = static_cast<uint8_t>(TFTravelErr::NoTerminal);
            }
            else
            {
                float dest[3]{0.0f, 0.0f, 0.0f};
                bool haveDest = false;
                if (req.mapId == kTFMapCindralWastes)
                {
                    const FactionId faction = m_ctx->serverSim->GetPlayerFaction(sender);
                    haveDest = ServerWarpgateOf(faction, dest);
                }
                else
                {
                    ServerSanctuaryPadOf(sender, dest);
                    haveDest = true;
                }

                if (!haveDest)
                    rep.reason = static_cast<uint8_t>(TFTravelErr::ServerError);
                else
                {
                    m_ctx->serverSim->TeleportPawn(sender, dest[0], dest[1], dest[2]);
                    m_mapOf[sender] = req.mapId;
                    ++m_travels;
                    rep.accepted = 1;
                    rep.reason = static_cast<uint8_t>(TFTravelErr::Ok);
                    rep.posX = dest[0];
                    rep.posY = dest[1];
                    rep.posZ = dest[2];
                    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u traveled to %s (%.0f, %.0f)", sender,
                                   TFTravel_MapName(req.mapId), dest[0], dest[2]);
                }
            }
        }

        if (!rep.accepted)
            ++m_travelsRejected;

        // Reply: in-process for the listen-host/standalone local player (it has
        // no NetworkManager socket — the TFServerSim::SendToPlayer precedent),
        // over the wire otherwise.
        if (sender == m_ctx->localPlayer)
        {
            ApplyTravelReply(rep);
            return;
        }
#ifdef ENABLE_NETWORKING
        ServerSendTo(sender, kTFTravelMsg_Reply, &rep, sizeof(rep));
#endif
    }

    void TFTravelSystem::ServerBuildContinentInfo(TF_ContinentInfo& out) const
    {
        out = TF_ContinentInfo{};
        out.mapId = kTFMapCindralWastes;
        if (m_ctx->players)
        {
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    const size_t f = static_cast<size_t>(p.faction);
                    if (f >= 4)
                        return;
                    if (TFTravel_MapIdAt(p.pos[0], p.pos[2]) == kTFMapCindralWastes && out.pop[f] < 0xFFFF)
                        ++out.pop[f];
                });
        }
        if (m_ctx->regions)
        {
            out.held[static_cast<size_t>(FactionId::MRA)] =
                static_cast<uint16_t>(m_ctx->regions->RegionsHeld(FactionId::MRA));
            out.held[static_cast<size_t>(FactionId::AUC)] =
                static_cast<uint16_t>(m_ctx->regions->RegionsHeld(FactionId::AUC));
            out.held[static_cast<size_t>(FactionId::HLX)] =
                static_cast<uint16_t>(m_ctx->regions->RegionsHeld(FactionId::HLX));
        }
    }

} // namespace Terrafront
