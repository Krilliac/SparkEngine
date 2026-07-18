/**
 * @file TFServerSimNetHandlers.cpp
 * @brief TFServerSim gameplay TFMsg handlers: input/spawn/fire/faction, vehicle
 *        seat ops, redeploy, continent-hop, chat routing and the spawn-reply/
 *        world-welcome sends (same class, split per repo file-size rules — see
 *        TFServerSim.cpp).
 */
#include "Net/TFServerSim.h"
#include "Net/TFServerSimConstants.h"

#include "Net/TFChatRules.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRedeployProtocol.h"
#include "Data/TFDataTables.h"
#include "World/TFRegionSystem.h"
#include "World/TFTravelSystem.h" // W13 multimap server-authoritative continent-hop: LookupContinentEndpoint
#include "World/TFWorldSetup.h"
#include "Game/TFMovementModel.h" // kTFEyeHeightM
#include "Game/TFOutfitSystem.h"  // Outfits lane: OutfitRequest routing + session hooks
#include "Game/TFPlayerSystem.h"
#include "Game/TFRedeployRules.h"    // W7 ui-map-keys: redeploy validation
#include "Game/TFServerValidation.h" // W13 anti-cheat lane: movement/fire-origin sanity (see file header)
#include "Game/TFSquadSystem.h"
#include "Game/TFVehicleSystem.h" // TF-W3: seat routing + seated-pawn ride sync
#include "Game/TFWeaponSystem.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace Terrafront
{

#ifdef ENABLE_NETWORKING

    void TFServerSim::HandleClientInput(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_ClientInput) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_ClientInput in;
        std::memcpy(&in, data, sizeof(in));
        EnqueueInput(sender, in);
    }

    void TFServerSim::HandleSpawnRequest(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_SpawnRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_SpawnRequest req;
        std::memcpy(&req, data, sizeof(req));

        // TF-W3 (vehicles agent): Aegis mobile-spawn requests (spawnKind==2) are
        // validated end-to-end by TFPlayerSystem (owns respawn records + the
        // GetAegisSpawnPos check) which also sends the TF_SpawnReply; the pawn it
        // spawns re-enters this sim via EvPlayerSpawned exactly like any other.
        // Region (1), Aegis (2) and squad-leader (3) spawns are validated
        // end-to-end by TFPlayerSystem (owns respawn records + point checks).
        if ((req.spawnKind == 1 || req.spawnKind == 2 || req.spawnKind == 3) && m_ctx->players)
        {
            m_ctx->players->ServerHandleSpawnRequest(sender, req);
            return;
        }

        TF_SpawnReply rep{};
        rep.accepted = 0;
        rep.reason = 1; // bad-point until proven otherwise

        const FactionId faction = GetPlayerFaction(sender);

        if (faction == FactionId::None)
        {
            rep.reason = 1; // must FactionSelect first
        }
        else if (req.classId >= static_cast<uint8_t>(ClassId::COUNT) ||
                 req.classId == static_cast<uint8_t>(ClassId::Colossus))
        {
            rep.reason = 4; // class-locked (Colossus is terminal-purchased, DESIGN §1)
        }
        else if (req.spawnKind != 0)
        {
            rep.reason = 1; // TF-W2: region spawns; TF-W3: aegis + squad-leader spawns
        }
        else if (m_move.contains(sender))
        {
            rep.reason = 1; // already alive
        }
        else if (auto dt = m_deathTime.find(sender);
                 dt != m_deathTime.end() && m_serverTime < dt->second + kRespawnDelaySec)
        {
            rep.reason = 2;
            rep.respawnDelay = static_cast<float>(dt->second + kRespawnDelaySec - m_serverTime);
        }
        else if (m_ctx->data && m_ctx->players)
        {
            // W1: spawnKind==0 → faction skyanchor home region from regions.json
            const RegionDef* home = nullptr;
            for (const RegionDef& r : m_ctx->data->GetContinent().regions)
            {
                if (r.tier == "skyanchor" && r.homeFaction == faction)
                {
                    home = &r;
                    break;
                }
            }
            if (home)
            {
                const float x = home->spawns.empty() ? home->centerX : (*home->spawns.begin())[0];
                const float z = home->spawns.empty() ? home->centerZ : (*home->spawns.begin())[1];
                const float y = m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f;
                const float yaw = std::atan2(kWorldMax * 0.5f - x, kWorldMax * 0.5f - z); // face map center
                const float pos[3]{x, y, z};

                const EntityId ent =
                    m_ctx->players->ServerSpawnPawn(sender, faction, static_cast<ClassId>(req.classId), pos, yaw);
                if (ent != 0)
                {
                    // Seed movement immediately (EvPlayerSpawned will re-seed identically).
                    MoveState ms;
                    ms.pawn = ent;
                    ms.cls = static_cast<ClassId>(req.classId);
                    ms.pos[0] = x;
                    ms.pos[1] = y;
                    ms.pos[2] = z;
                    ms.yaw = yaw;
                    m_move[sender] = ms;
                    m_deathTime.erase(sender);

                    rep.accepted = 1;
                    rep.reason = 0;
                    rep.entityId = ent;
                    rep.posX = x;
                    rep.posY = y;
                    rep.posZ = z;
                }
            }
        }

        SendSpawnReply(sender, rep);
    }

    void TFServerSim::HandleFireEvent(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_FireEvent) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        if (!m_ctx->weapons)
            return;
        TF_FireEvent ev;
        std::memcpy(&ev, data, sizeof(ev));

        // W13 anti-cheat lane (position sanity, item 3): reject a fire event
        // whose CLAIMED muzzle position diverges wildly from the server's own
        // trusted pawn position, before it ever reaches TFWeaponSystem.
        // ServerHandleFire already never trusts ev.origin* for damage/hit
        // resolution EXCEPT as a fallback when the pawn registry reports the
        // zeroed stub position — that exact condition is mirrored below so
        // this can never false-reject the legitimate fallback path. A gross
        // divergence otherwise has no legitimate cause (network latency /
        // client-side prediction drift is well under a meter) and is a strong
        // signal of a modified client lying about where it is.
        if (m_ctx->players)
        {
            PawnInfo pawn;
            if (m_ctx->players->GetPawnByPlayer(sender, pawn) && pawn.alive &&
                !(pawn.pos[0] == 0.0f && pawn.pos[1] == 0.0f && pawn.pos[2] == 0.0f))
            {
                const float trusted[3] = {pawn.pos[0], pawn.pos[1] + kTFEyeHeightM, pawn.pos[2]};
                const float claimed[3] = {ev.originX, ev.originY, ev.originZ};
                if (!TFServerValidation::Get().CheckFireOrigin(sender, claimed, trusted, kMaxFireOriginDivergenceM))
                    return; // dropped: claimed origin too far from the server's known pawn position
            }
        }

        m_ctx->weapons->ServerHandleFire(sender, ev);
    }

    // TF-W3 (vehicles agent): thin size-validated routes into TFVehicleSystem.
    void TFServerSim::HandleVehicleSeatOp(PlayerId sender, const void* data, size_t size, bool enter)
    {
        if (size != sizeof(TF_VehicleSeatOp) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        if (!m_ctx->vehicles)
            return;
        TF_VehicleSeatOp op;
        std::memcpy(&op, data, sizeof(op));
        m_ctx->vehicles->ServerHandleSeatOp(sender, op, enter);
    }

    void TFServerSim::HandleAegisDeploy(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_AegisDeploy) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        if (!m_ctx->vehicles)
            return;
        TF_AegisDeploy msg;
        std::memcpy(&msg, data, sizeof(msg));
        m_ctx->vehicles->ServerHandleAegisDeploy(sender, msg);
    }

    void TFServerSim::HandleFactionSelect(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_FactionSelect) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_FactionSelect sel;
        std::memcpy(&sel, data, sizeof(sel));

        if (m_move.contains(sender))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] player %u tried to switch faction while alive — ignored",
                           sender);
            return;
        }
        SetPlayerFaction(sender, static_cast<FactionId>(sel.faction));
    }

    // W7 ui-map-keys: alive-pawn redeploy — server-authoritative teleport to a
    // friendly, non-contested, lattice-linked region's spawn point. Countdown
    // is client UX only; the anti-abuse control here is the per-player interval.
    void TFServerSim::HandleRedeployRequest(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_RedeployRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_RedeployRequest rq;
        std::memcpy(&rq, data, sizeof(rq));

        TF_RedeployReply rep{};
        rep.regionId = rq.regionId;

        const FactionId faction = GetPlayerFaction(sender);
        rep.reason = TFRedeployRules::CanRedeploy(*m_ctx, sender, faction, rq.regionId);

        if (rep.reason == kTFRedeployOk)
        {
            if (auto it = m_redeployNextAt.find(sender); it != m_redeployNextAt.end() && m_serverTime < it->second)
            {
                rep.reason = kTFRedeployCooldown;
                rep.cooldownSec = static_cast<float>(it->second - m_serverTime);
            }
        }

        float pos[3]{};
        float yaw = 0.0f;
        if (rep.reason == kTFRedeployOk && !TFRedeployRules::ResolveSpawnPos(*m_ctx, rq.regionId, faction, pos, yaw))
            rep.reason = kTFRedeployBadRegion;

        if (rep.reason == kTFRedeployOk)
        {
            TeleportPawn(sender, pos[0], pos[1], pos[2]);
            m_redeployNextAt[sender] = m_serverTime + TFRedeployRules::kTFRedeployIntervalSec;
            rep.accepted = 1;
            rep.posX = pos[0];
            rep.posY = pos[1];
            rep.posZ = pos[2];
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] redeploy: p%u -> region %u", sender,
                           static_cast<unsigned>(rq.regionId));
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::RedeployReply), &rep, sizeof(rep), true);
    }

    // W13 multimap server-authoritative continent-hop (docs/TERRAFRONT_MULTIMAP.md
    // §2.2): answer TF_ContinentHopRequest from THIS process's own registry
    // (World/TFTravelSystem.h, sourced from THIS server's continents.json) —
    // never from anything the requesting client asserts. This is the
    // load-bearing trust-boundary fix the doc's "what W13 actually ships"
    // section flagged as missing: previously the CLIENT read its own
    // continents.json copy and self-served the endpoint.
    void TFServerSim::HandleContinentHopRequest(PlayerId sender, const void* data, size_t size)
    {
        if (data == nullptr || size != sizeof(TF_ContinentHopRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_ContinentHopRequest req{};
        std::memcpy(&req, data, sizeof(req));

        TF_ContinentHopReply rep{};
        rep.mapId = req.mapId;
        rep.ok = 0;
        rep.port = 0;
        rep.host[0] = '\0';

        std::string host;
        uint16_t port = 0;
        if (m_ctx->travel && m_ctx->travel->LookupContinentEndpoint(req.mapId, host, port))
        {
            // Truncate rather than reject an oversized operator-configured
            // hostname — the wire field is a fixed 64 bytes (DNS names can
            // exceed that; IPs/short hostnames never will in practice).
            const size_t n = std::min(host.size(), sizeof(rep.host) - 1);
            std::memcpy(rep.host, host.data(), n);
            rep.host[n] = '\0';
            rep.port = port;
            rep.ok = 1;
        }

        if (rep.ok)
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u continent-hop request: mapId %u -> %s:%u", sender,
                           static_cast<unsigned>(req.mapId), rep.host, static_cast<unsigned>(rep.port));
        else
            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "[TF] player %u continent-hop request: mapId %u -> no server configured", sender,
                           static_cast<unsigned>(req.mapId));

        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::ContinentHopReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::SendSpawnReply(PlayerId player, const TF_SpawnReply& reply)
    {
        SendToPlayer(player, static_cast<uint16_t>(TFMsg::SpawnReply), &reply, sizeof(reply), true);
    }

    void TFServerSim::SendWorldWelcome(PlayerId player)
    {
        TF_WorldWelcome w{};
        w.yourPlayerId = player;
        w.yourFaction = static_cast<uint8_t>(GetPlayerFaction(player));
        if (m_ctx->data && m_ctx->data->IsLoaded())
        {
            const size_t n = m_ctx->data->GetContinent().regions.size();
            w.regionCount = static_cast<uint8_t>(std::min<size_t>(n, 255));
        }
        w.territoryHash = m_ctx->regions ? m_ctx->regions->TerritoryHash() : 0;
        w.serverTimeMs = static_cast<uint32_t>(m_serverTime * 1000.0);
        SendToPlayer(player, static_cast<uint16_t>(TFMsg::WorldWelcome), &w, sizeof(w), true);
    }

    RegionId TFServerSim::RegionOfPlayer(PlayerId player) const
    {
        if (!m_ctx || !m_ctx->players || !m_ctx->data || !m_ctx->data->IsLoaded())
            return kInvalidRegion;
        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn))
            return kInvalidRegion;

        RegionId nearest = kInvalidRegion;
        float bestSq = std::numeric_limits<float>::max();
        for (const RegionDef& region : m_ctx->data->GetContinent().regions)
        {
            const float dx = pawn.pos[0] - region.centerX;
            const float dz = pawn.pos[2] - region.centerZ;
            const float d2 = dx * dx + dz * dz;
            if (d2 < bestSq)
            {
                bestSq = d2;
                nearest = region.id;
            }
        }
        return nearest;
    }

    void TFServerSim::HandleChatMsg(PlayerId sender, const void* data, size_t size)
    {
        if (data == nullptr || size != sizeof(TF_ChatMsg) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }

        TF_ChatMsg incoming{};
        std::memcpy(&incoming, data, sizeof(incoming));
        if (!IsValidChatChannel(incoming.channel))
        {
            ++m_badPackets;
            return;
        }

        const double nextAllowed = m_chatNextAt[sender];
        if (m_serverTime < nextAllowed)
            return;
        // Consume the cooldown before text normalization so empty/control-only
        // packets cannot bypass the limiter and hammer the server parser.
        m_chatNextAt[sender] = m_serverTime + 0.5;

        TF_ChatMsg outgoing{};
        outgoing.fromPlayer = sender;
        outgoing.channel = incoming.channel;
        if (!NormalizeChatText(incoming.text, sizeof(incoming.text), outgoing.text, sizeof(outgoing.text)))
            return;

        const auto channel = static_cast<ChatChannel>(outgoing.channel);
        const FactionId senderFaction = GetPlayerFaction(sender);
        const SquadId senderSquad = m_ctx->squads ? m_ctx->squads->SquadOf(sender) : kInvalidSquad;
        const RegionId senderRegion = RegionOfPlayer(sender);
        // W8 ui-polish: outfit channel membership (authority registry truth).
        const uint32_t senderOutfit = m_ctx->outfits ? m_ctx->outfits->OutfitIdOf(sender) : 0u;
        PawnInfo senderPawn{};
        const bool senderHasPawn = m_ctx->players && m_ctx->players->GetPawnByPlayer(sender, senderPawn);

        size_t recipients = 0;
        for (const PlayerId recipient : m_enteredWorld)
        {
            PawnInfo recipientPawn{};
            const bool recipientHasPawn = m_ctx->players && m_ctx->players->GetPawnByPlayer(recipient, recipientPawn);
            float distanceSq = 0.0f;
            if (senderHasPawn && recipientHasPawn)
            {
                const float dx = senderPawn.pos[0] - recipientPawn.pos[0];
                const float dy = senderPawn.pos[1] - recipientPawn.pos[1];
                const float dz = senderPawn.pos[2] - recipientPawn.pos[2];
                distanceSq = dx * dx + dy * dy + dz * dz;
            }

            const FactionId recipientFaction = GetPlayerFaction(recipient);
            const SquadId recipientSquad = m_ctx->squads ? m_ctx->squads->SquadOf(recipient) : kInvalidSquad;
            const RegionId recipientRegion = RegionOfPlayer(recipient);
            const TFChatRouteView view{
                recipient == sender,
                senderRegion != kInvalidRegion && recipientRegion == senderRegion,
                senderFaction != FactionId::None && recipientFaction == senderFaction,
                senderSquad != kInvalidSquad && recipientSquad == senderSquad,
                senderHasPawn && recipientHasPawn,
                distanceSq,
                senderOutfit != 0u && m_ctx->outfits->OutfitIdOf(recipient) == senderOutfit,
            };
            if (!ShouldReceiveChat(channel, view))
                continue;
            SendToPlayer(recipient, static_cast<uint16_t>(TFMsg::ChatMsg), &outgoing, sizeof(outgoing), true);
            ++recipients;
        }

        SPARK_LOG_TRACE(Spark::LogCategory::Game, "[TF] chat %s from p%u routed to %zu player(s)",
                        ChatChannelName(channel), sender, recipients);
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
