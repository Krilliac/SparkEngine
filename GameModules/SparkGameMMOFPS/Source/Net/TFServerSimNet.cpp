/**
 * @file TFServerSimNet.cpp
 * @brief TFServerSim session/socket plumbing: TFMsg handler registration, client
 *        join/leave polling, anti-cheat kick enforcement, per-player session
 *        cleanup and the owner-directed send paths (same class, split per repo
 *        file-size rules — see TFServerSim.cpp).
 */
#include "Net/TFServerSim.h"

#include "Account/TFAccountSystem.h"   // W5 onboarding (Task 4)
#include "Account/TFCharacterSystem.h" // W5 onboarding (Task 4)
#include "Net/TFClientNet.h"           // W5 onboarding (Task 7): local-player reply loopback
#include "Net/TFNetworkLifecycle.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFRepProtocol.h" // kTFRepMsg_MoveState + TF_MoveState
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h" // W5 onboarding (Task 6): disconnect progress flush
#include "Game/TFDirectiveSystem.h"   // W6 directives: disconnect progress sweep
#include "Game/TFOutfitSystem.h"      // Outfits lane: OutfitRequest routing + session hooks
#include "Game/TFServerValidation.h"  // W13 anti-cheat lane: movement/fire-origin sanity (see file header)
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>
#include <vector>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Networking (server-side TFMsg routing)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    void TFServerSim::RegisterNetHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        auto route = [&nm](TFMsg id, auto&& fn)
        { nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)), std::forward<decltype(fn)>(fn)); };

        // W5 T6 (T4-review #1 security fix): gameplay ids are now routed through
        // RouteClientMessage — the SAME single choke point the onboarding ids use
        // below and the listen-host/standalone loopback path uses
        // (TFClientNet::RouteLoopback) — so the enter-world gate added there
        // applies uniformly to every client-originated gameplay message,
        // regardless of transport.
        // final-review #3 (gate defense-in-depth): VehicleEnter/VehicleExit/
        // AegisDeploy/SquadMsg used to be direct routes straight into their
        // handlers below, bypassing the RouteClientMessage enter-world gate
        // entirely -- an unauthenticated/pre-enter-world client could seat a
        // vehicle, toggle Aegis, or spam squad ops. They now go through the same
        // choke point as the other gameplay ids.
        for (TFMsg id :
             {TFMsg::ClientInput, TFMsg::SpawnRequest, TFMsg::FireEvent, TFMsg::FactionSelect, TFMsg::VehicleEnter,
              TFMsg::VehicleExit, TFMsg::AegisDeploy, TFMsg::SquadMsg, TFMsg::RedeployRequest})
        {
            route(id, [this, id](const NetworkMessage& m)
                  { RouteClientMessage(m.senderID, id, m.payload.data(), m.payload.size()); });
        }

        // W6 progression: loadout persistence + unlock-tree purchases now route
        // through the same enter-world-gated choke point as the gameplay ids.
        for (TFMsg id : {TFMsg::LoadoutChange, TFMsg::UnlockRequest})
        {
            route(id, [this, id](const NetworkMessage& m)
                  { RouteClientMessage(m.senderID, id, m.payload.data(), m.payload.size()); });
        }

        // Outfits lane: enter-world-gated like the other gameplay ids.
        route(TFMsg::OutfitRequest, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::OutfitRequest, m.payload.data(), m.payload.size()); });

        // class-abilities lane (W9): enter-world-gated like the other gameplay ids.
        route(TFMsg::AbilityRequest, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::AbilityRequest, m.payload.data(), m.payload.size()); });

        // grenades lane (W10): enter-world-gated like the other gameplay ids.
        route(TFMsg::GrenadeThrow, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::GrenadeThrow, m.payload.data(), m.payload.size()); });

        // ping-system lane (W11): enter-world-gated like the other gameplay ids.
        route(TFMsg::PingPlace, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::PingPlace, m.payload.data(), m.payload.size()); });

        route(TFMsg::ChatMsg, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::ChatMsg, m.payload.data(), m.payload.size()); });

        // W5 onboarding (Task 4): login -> char-select/create/delete -> enter-world.
        // Routed through RouteClientMessage so the socket path and the listen-host/
        // standalone loopback path (TFClientNet::RouteLoopback) share one dispatch.
        for (TFMsg id : {TFMsg::LoginRequest, TFMsg::RegisterRequest})
        {
            nm.RegisterSensitiveHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                                        [this, id](const NetworkMessage& m)
                                        { RouteClientMessage(m.senderID, id, m.payload.data(), m.payload.size()); });
        }
        for (TFMsg id : {TFMsg::CharListRequest, TFMsg::CharCreateReq, TFMsg::CharDeleteReq, TFMsg::EnterWorldReq})
        {
            route(id, [this, id](const NetworkMessage& m)
                  { RouteClientMessage(m.senderID, id, m.payload.data(), m.payload.size()); });
        }

        // W13 multimap server-authoritative continent-hop (docs/TERRAFRONT_
        // MULTIMAP.md §2.2): enter-world-gated like the other post-onboarding
        // gameplay ids (only sent from the sanctuary terminal).
        route(TFMsg::ContinentHopRequest, [this](const NetworkMessage& m)
              { RouteClientMessage(m.senderID, TFMsg::ContinentHopRequest, m.payload.data(), m.payload.size()); });

        m_handlersRegistered = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] server TFMsg handlers registered");
    }

    void TFServerSim::UnregisterNetHandlers()
    {
        // NetworkManager has no per-type removal; replace our handlers with no-ops
        // so no dangling `this` survives module shutdown.
        using Spark::Net::MessageType;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (TFMsg id :
             {TFMsg::ClientInput,     TFMsg::SpawnRequest,    TFMsg::FireEvent,          TFMsg::FactionSelect,
              TFMsg::LoadoutChange,   TFMsg::UnlockRequest,   TFMsg::SquadMsg,           TFMsg::ChatMsg,
              TFMsg::VehicleEnter,    TFMsg::VehicleExit,     TFMsg::AegisDeploy,        TFMsg::LoginRequest,
              TFMsg::RegisterRequest, TFMsg::CharListRequest, TFMsg::CharCreateReq,      TFMsg::CharDeleteReq,
              TFMsg::EnterWorldReq,   TFMsg::RedeployRequest, TFMsg::OutfitRequest,      TFMsg::AbilityRequest,
              TFMsg::GrenadeThrow,    TFMsg::PingPlace,       TFMsg::ContinentHopRequest})
        {
            nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                               [](const Spark::Net::NetworkMessage&) {});
        }
        m_handlersRegistered = false;
    }

    void TFServerSim::PrepareNetworkStop()
    {
        std::vector<PlayerId> additionalSessions;
        additionalSessions.reserve(m_enteredWorld.size() + m_activeCharacter.size() + 1);
        for (const PlayerId player : m_enteredWorld)
            additionalSessions.push_back(player);
        for (const auto& [player, character] : m_activeCharacter)
        {
            (void)character;
            additionalSessions.push_back(player);
        }
        if (m_ctx && m_ctx->IsAuthority())
            additionalSessions.push_back(m_ctx->localPlayer);

        StopNetworkSessionLifecycle(
            m_knownClients, m_handlersRegistered, additionalSessions,
            [this](PlayerId player) { CleanupPlayerSession(player); }, [this] { UnregisterNetHandlers(); });
    }

    void TFServerSim::PollClientJoinsLeaves()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const auto& clients = nm.GetClients();

        for (const auto& [id, info] : clients)
        {
            if (info.state == Spark::Net::ConnectionState::Connected && !m_knownClients.contains(id))
            {
                // W5 onboarding gate: TF_WorldWelcome is NO LONGER sent on connect.
                // It is now sent only from HandleEnterWorld, after a successful
                // login + character enter-world. See DESIGN.md W5.
                m_knownClients.insert(id);
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client %u joined — awaiting login/enter-world", id);
            }
        }

        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            const PlayerId id = *it;
            const auto clientIt = clients.find(id);
            if (clientIt == clients.end() || clientIt->second.state != Spark::Net::ConnectionState::Connected)
            {
                CleanupPlayerSession(id);
                it = m_knownClients.erase(it);
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client %u left — pawn cleaned up", id);
            }
            else
            {
                ++it;
            }
        }
    }

    void TFServerSim::EnforceAntiCheatKicks()
    {
        // Two-pass: CleanupPlayerSession() erases the m_move entry for a kicked
        // player, so mutating m_move while iterating it here would invalidate
        // the iterator. Collect offenders first, act after.
        std::vector<PlayerId> toKick;
        for (const auto& [player, ms] : m_move)
        {
            if (TFServerValidation::Get().ShouldKick(player))
                toKick.push_back(player);
        }
        if (toKick.empty())
            return;

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (PlayerId player : toKick)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF-anticheat] player %u crossed the kick threshold (violation score %u) -- "
                           "disconnecting",
                           player, TFServerValidation::Get().ViolationScore(player));
            nm.KickClient(player, "Anti-cheat: excessive validation failures");
            // Same teardown a real socket drop runs (PollClientJoinsLeaves) --
            // don't wait for the next poll to notice the disconnect.
            CleanupPlayerSession(player);
            m_knownClients.erase(player);
        }
    }

    void TFServerSim::CleanupPlayerSession(PlayerId id)
    {
        auto mv = m_move.find(id);
        if (mv != m_move.end() && m_ctx->players)
            m_ctx->players->ServerKillPawn(mv->second.pawn, kInvalidPlayer, kInvalidWeapon, false);
        m_move.erase(id);
        m_inputs.erase(id);
        m_factions.erase(id);
        m_deathTime.erase(id);
        m_enteredWorld.erase(id);
        m_chatNextAt.erase(id);
        m_redeployNextAt.erase(id); // W7 ui-map-keys
        // W13 anti-cheat lane: same recycled-PlayerId hygiene as the
        // progression/directives/outfits ClearPlayer calls below — a fresh
        // client reusing a freed PlayerId must not inherit a stranger's
        // violation history.
        TFServerValidation::Get().ClearPlayer(id);
        // W5 onboarding (Task 6): flush the active character's progress
        // one last time before dropping the session (DESIGN.md W5 "Error
        // handling": "On disconnect, flush the active character's
        // progress") — the periodic TFProgressionSystem::SaveNow debounce
        // could otherwise miss a few seconds of the final session.
        if (auto cIt = m_activeCharacter.find(id); cIt != m_activeCharacter.end())
        {
            if (m_ctx->characters && m_ctx->progression)
            {
                const bool persisted =
                    m_ctx->characters->PersistProgress(cIt->second, m_ctx->progression->XPOf(id),
                                                       m_ctx->progression->RankOf(id), m_ctx->progression->FluxOf(id));
                if (!persisted && !m_ctx->progression->SaveNow())
                    SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                    "[TF] final progression persistence failed for disconnected player %u", id);
            }
            m_activeCharacter.erase(cIt);
        }
        // final-review #2 (leak): drop the runtime progression record for
        // this PlayerId AFTER the final flush-to-character above. Without
        // this, a recycled PlayerId (a new client reusing a freed slot)
        // would inherit the prior occupant's xp/rank/flux and leak them
        // onto a different account's character.
        if (m_ctx->progression)
            m_ctx->progression->ClearPlayer(id);
        // W6 directives: same recycled-PlayerId hygiene for directive progress.
        if (m_ctx->directives)
            m_ctx->directives->ClearPlayer(id);
        // Outfits lane: drop the player->charId binding + roster/tag interest.
        if (m_ctx->outfits)
            m_ctx->outfits->ServerOnPlayerLeft(id);
        if (m_ctx->account)
            m_ctx->account->ClearSession(id);
    }

    void TFServerSim::DebugSimulateDisconnect(PlayerId player)
    {
        // Test-only hook (final-review #1/#2 regression proof): the listen-host/
        // standalone loopback player never appears in m_knownClients (see
        // SendToPlayer above), so PollClientJoinsLeaves's real-socket-drop
        // detection never fires for it. Run the identical cleanup directly so
        // tf_selftest_onboarding can prove a disconnect -> re-login -> re-enter
        // round-trip without tearing down the whole NetworkManager/session.
        CleanupPlayerSession(player);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] simulated disconnect cleanup for player %u (test hook)", player);
    }

    void TFServerSim::SendToPlayer(PlayerId player, uint16_t msgId, const void* payload, size_t size, bool reliable)
    {
        // W5 T7 (acceptance-harness fix): the listen-host/standalone local player
        // (m_ctx->localPlayer == kTFLocalHostPlayer) never establishes a real
        // NetworkManager socket -- TFClientNet::RouteLoopback bypasses the socket
        // entirely for the C->S direction, calling RouteClientMessage directly --
        // so below, nm.SendToClient() would look this id up in m_clientAddresses,
        // find nothing, and silently drop the reply (see NetworkConnection.cpp
        // SendToClient). Every onboarding client-state transition (logged-in,
        // character list, in-world) is driven purely by these S->C replies; there
        // is no ECS ground truth for "logged in"/"in world" the local player
        // could read directly the way movement/spawn state is (TFClientNet reads
        // the authoritative Transform for those). That silent drop broke the
        // whole login->world flow for local/standalone play. Mirror the reply
        // straight into the in-process client for that one player instead.
        if (player == m_ctx->localPlayer && m_ctx->clientNet && !m_knownClients.contains(player))
        {
            m_ctx->clientNet->DeliverLoopbackReply(static_cast<TFMsg>(msgId), payload, size);
            return;
        }

        auto& nm = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = reliable ? Spark::Net::ChannelType::Reliable : Spark::Net::ChannelType::Unreliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(player, msg);
    }

    void TFServerSim::SendMoveStates()
    {
        for (const auto& [player, ms] : m_move)
        {
            if (!m_knownClients.contains(player))
                continue; // e.g. listen-host local player is not a network client

            TF_MoveState st{};
            st.lastAckedSeq = ms.lastSeq;
            st.posX = ms.pos[0];
            st.posY = ms.pos[1];
            st.posZ = ms.pos[2];
            st.velX = ms.vel[0];
            st.velY = ms.vel[1];
            st.velZ = ms.vel[2];
            st.yaw = ms.yaw;
            st.pitch = ms.pitch;
            st.grounded = ms.grounded ? 1 : 0;
            SendToPlayer(player, kTFRepMsg_MoveState, &st, sizeof(st), false);
        }
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
