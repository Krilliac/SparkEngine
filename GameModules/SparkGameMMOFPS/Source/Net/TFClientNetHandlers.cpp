/**
 * @file TFClientNetHandlers.cpp
 * @brief TFClientNet client-side TFMsg handlers: registration/release against
 *        NetworkManager, the in-process loopback reply mirror and the S->C
 *        reply handlers (world-welcome, spawn, combat feedback, chat and the
 *        W5 onboarding replies). Core pump/prediction logic lives in
 *        TFClientNet.cpp (same class, split per repo file-size rules —
 *        mirrors the TFWeaponSystem/TFWeaponServer split); the loopback
 *        router lives in TFClientNetLoopback.cpp and the view path in
 *        TFClientNetView.cpp.
 */
#include "Net/TFClientNet.h"
#include "Net/TFChatRules.h"

#include "Game/TFPlayerSystem.h"
#include "Net/TFRedeployProtocol.h" // W7 ui-map-keys: redeploy reply -> map screen
#include "UI/TFHUD.h"
#include "UI/TFMapScreen.h" // W7 ui-map-keys: OnRedeployReply sink
#include "UI/TFLoginFlow.h" // W5 onboarding (Task 6): direct reply-sink forwarding
#include "UI/TFScoreboard.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Client-side TFMsg handlers
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    void TFClientNet::RegisterClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        auto route = [&nm](TFMsg id, auto&& fn)
        { nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)), std::forward<decltype(fn)>(fn)); };

        route(TFMsg::WorldWelcome,
              [this](const NetworkMessage& m) { OnWorldWelcome(m.payload.data(), m.payload.size()); });
        route(TFMsg::SpawnReply, [this](const NetworkMessage& m) { OnSpawnReply(m.payload.data(), m.payload.size()); });
        route(TFMsg::HitConfirm, [this](const NetworkMessage& m) { OnHitConfirm(m.payload.data(), m.payload.size()); });
        route(TFMsg::DamageEvent,
              [this](const NetworkMessage& m) { OnDamageEvent(m.payload.data(), m.payload.size()); });
        route(TFMsg::KillEvent, [this](const NetworkMessage& m) { OnKillEvent(m.payload.data(), m.payload.size()); });
        route(TFMsg::XPEvent, [this](const NetworkMessage& m) { OnXPEvent(m.payload.data(), m.payload.size()); });
        route(TFMsg::ChatMsg, [this](const NetworkMessage& m) { OnChatMsg(m.payload.data(), m.payload.size()); });

        // W7 ui-map-keys: server-validated redeploy reply -> map screen.
        route(TFMsg::RedeployReply,
              [this](const NetworkMessage& m)
              {
                  if (m.payload.size() == sizeof(TF_RedeployReply) && m_ctx->map)
                  {
                      TF_RedeployReply rep;
                      std::memcpy(&rep, m.payload.data(), sizeof(rep));
                      m_ctx->map->OnRedeployReply(rep);
                  }
              });

        // Accepted-but-unrouted W2 broadcasts (no "unknown message" warnings):
        for (TFMsg id : {TFMsg::RegionState, TFMsg::CaptureTick, TFMsg::SquadMsg})
            route(id, [](const NetworkMessage&) {});

        // W5 onboarding (Task 4): login/register/char-CRUD replies. TF_WorldWelcome
        // (already routed above) is the enter-world reply — no separate message id.
        route(TFMsg::LoginReply, [this](const NetworkMessage& m) { OnLoginReply(m.payload.data(), m.payload.size()); });
        route(TFMsg::RegisterReply,
              [this](const NetworkMessage& m) { OnRegisterReply(m.payload.data(), m.payload.size()); });
        route(TFMsg::CharListReply,
              [this](const NetworkMessage& m) { OnCharListReply(m.payload.data(), m.payload.size()); });
        route(TFMsg::CharCreateReply,
              [this](const NetworkMessage& m) { OnCharCreateReply(m.payload.data(), m.payload.size()); });
        route(TFMsg::CharDeleteReply,
              [this](const NetworkMessage& m) { OnCharDeleteReply(m.payload.data(), m.payload.size()); });

        m_handlersRegistered = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client TFMsg handlers registered");
    }

    void TFClientNet::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace our handlers with no-ops
        // so no dangling `this` survives module shutdown (TFServerSim pattern).
        using Spark::Net::MessageType;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (TFMsg id : {TFMsg::WorldWelcome, TFMsg::SpawnReply, TFMsg::HitConfirm, TFMsg::DamageEvent,
                         TFMsg::KillEvent, TFMsg::XPEvent, TFMsg::RegionState, TFMsg::CaptureTick, TFMsg::ChatMsg,
                         TFMsg::SquadMsg, TFMsg::LoginReply, TFMsg::RegisterReply, TFMsg::CharListReply,
                         TFMsg::CharCreateReply, TFMsg::CharDeleteReply, TFMsg::RedeployReply})
        {
            nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                               [](const Spark::Net::NetworkMessage&) {});
        }
        m_handlersRegistered = false;
    }

    void TFClientNet::DeliverLoopbackReply(TFMsg id, const void* data, size_t size)
    {
        // W5 onboarding (Task 7 acceptance-harness fix): in-process mirror of
        // RegisterClientHandlers' route table, invoked by TFServerSim::
        // SendToPlayer for the listen-host/standalone local player instead of a
        // socket delivery that would otherwise never arrive (see the header
        // comment on this method). Every S->C id the local player can receive is
        // covered here so the reply-driven onboarding state machine
        // (TFClientNet's stash + TFLoginFlow) works identically for local and
        // networked play.
        switch (id)
        {
        case TFMsg::WorldWelcome:
            OnWorldWelcome(data, size);
            break;
        case TFMsg::SpawnReply:
            OnSpawnReply(data, size);
            break;
        case TFMsg::HitConfirm:
            OnHitConfirm(data, size);
            break;
        case TFMsg::DamageEvent:
            OnDamageEvent(data, size);
            break;
        case TFMsg::KillEvent:
            OnKillEvent(data, size);
            break;
        case TFMsg::XPEvent:
            OnXPEvent(data, size);
            break;
        case TFMsg::ChatMsg:
            OnChatMsg(data, size);
            break;
        case TFMsg::LoginReply:
            OnLoginReply(data, size);
            break;
        case TFMsg::RegisterReply:
            OnRegisterReply(data, size);
            break;
        case TFMsg::CharListReply:
            OnCharListReply(data, size);
            break;
        case TFMsg::CharCreateReply:
            OnCharCreateReply(data, size);
            break;
        case TFMsg::CharDeleteReply:
            OnCharDeleteReply(data, size);
            break;
        case TFMsg::RedeployReply: // W7 ui-map-keys
            if (size == sizeof(TF_RedeployReply) && m_ctx->map)
            {
                TF_RedeployReply rep;
                std::memcpy(&rep, data, sizeof(rep));
                m_ctx->map->OnRedeployReply(rep);
            }
            break;
        default:
            break;
        }
    }

    void TFClientNet::OnChatMsg(const void* data, size_t size)
    {
        if (size != sizeof(TF_ChatMsg))
            return;
        TF_ChatMsg msg{};
        std::memcpy(&msg, data, sizeof(msg));
        if (!IsValidChatChannel(msg.channel))
            return;

        char normalized[sizeof(msg.text)]{};
        if (!NormalizeChatText(msg.text, sizeof(msg.text), normalized, sizeof(normalized)))
            return;
        m_chatHistory.push_back(ChatLine{
            msg.fromPlayer,
            static_cast<ChatChannel>(msg.channel),
            normalized,
            kTFChatVisibleSec,
        });
        while (m_chatHistory.size() > kTFChatHistoryMax)
            m_chatHistory.pop_front();
    }

    void TFClientNet::OnWorldWelcome(const void* data, size_t size)
    {
        if (size != sizeof(TF_WorldWelcome))
            return;
        TF_WorldWelcome w;
        std::memcpy(&w, data, sizeof(w));

        m_localPlayer = w.yourPlayerId;
        m_ctx->localPlayer = w.yourPlayerId;
        if (w.yourFaction != 0)
            m_ctx->localFaction = static_cast<FactionId>(w.yourFaction);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] WorldWelcome: player %u, faction %s, %u regions, server t=%ums",
                       w.yourPlayerId, FactionTag(static_cast<FactionId>(w.yourFaction)),
                       static_cast<unsigned>(w.regionCount), w.serverTimeMs);

        // Faction picked before the connection existed: assert it now.
        if (w.yourFaction == 0 && m_ctx->localFaction != FactionId::None)
        {
            TF_FactionSelect sel{};
            sel.faction = static_cast<uint8_t>(m_ctx->localFaction);
            SendMsg(TFMsg::FactionSelect, &sel, sizeof(sel));
        }

        // W5 onboarding (Task 6): TF_WorldWelcome is now gated (see DESIGN.md W5)
        // to fire ONLY after a successful TFCharacterSystem::EnterWorld, so its
        // receipt IS the "entered world" signal — flip the context flag every
        // gameplay/UI system gates on and forward to TFLoginFlow's reply sink so
        // it can leave its EnteringWorld splash for InWorld.
        m_ctx->inWorld = true;
        if (m_ctx->loginFlow)
            m_ctx->loginFlow->OnEnteredWorld();
    }

    void TFClientNet::OnSpawnReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_SpawnReply))
            return;
        TF_SpawnReply rep;
        std::memcpy(&rep, data, sizeof(rep));

        if (m_ctx->players)
            m_ctx->players->ClientOnSpawnReply(rep);

        if (rep.accepted)
        {
            const float pos[3] = {rep.posX, rep.posY, rep.posZ};
            SeedPredictionAt(pos);
            if (m_ctx->hud)
                m_ctx->hud->SetRespawnState(false, 0.0f);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] spawn accepted: entity %u at (%.0f %.0f %.0f)", rep.entityId,
                           rep.posX, rep.posY, rep.posZ);
        }
        else if (m_ctx->hud)
        {
            if (rep.reason == 2) // respawn timer running
                m_ctx->hud->SetRespawnState(true, rep.respawnDelay);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] spawn rejected (reason %u)",
                           static_cast<unsigned>(rep.reason));
        }
    }

    void TFClientNet::OnHitConfirm(const void* data, size_t size)
    {
        if (size != sizeof(TF_HitConfirm))
            return;
        TF_HitConfirm hc;
        std::memcpy(&hc, data, sizeof(hc));
        if (m_ctx->hud)
            m_ctx->hud->ShowHitmarker(hc.killed != 0);
    }

    void TFClientNet::OnDamageEvent(const void* data, size_t size)
    {
        if (size != sizeof(TF_DamageEvent))
            return;
        TF_DamageEvent de;
        std::memcpy(&de, data, sizeof(de));

        if (m_ctx->hud)
            m_ctx->hud->ShowDamageFrom(de.dirOctant);

        if (m_events && m_ctx->players)
        {
            PawnInfo pawn{};
            const EntityId victim = m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) ? pawn.entity : 0;
            m_events->Fire(EvPlayerDamaged{victim, de.attackerEntity, static_cast<float>(de.damage), de.damageKind});
        }
    }

    void TFClientNet::OnKillEvent(const void* data, size_t size)
    {
        if (size != sizeof(TF_KillEvent))
            return;
        TF_KillEvent ke;
        std::memcpy(&ke, data, sizeof(ke));

        PushKillfeedEntry(ke.killerPlayer, ke.victimPlayer, ke.weaponId, static_cast<FactionId>(ke.killerFaction),
                          static_cast<FactionId>(ke.victimFaction), ke.headshot != 0);
        if (m_ctx->scoreboard)
            m_ctx->scoreboard->ClientNoteKill(ke);

        if (ke.victimPlayer == m_ctx->localPlayer && m_events)
            m_events->Fire(EvLocalPlayerDied{m_ctx->localPlayer, kTFRespawnDelaySec});
    }

    void TFClientNet::OnXPEvent(const void* data, size_t size)
    {
        if (size != sizeof(TF_XPEvent))
            return;
        TF_XPEvent xp;
        std::memcpy(&xp, data, sizeof(xp));
        m_lastRank = xp.newRank;
        m_lastXPTotal = xp.newTotalXP; // TF-W2: progression screen consumes this
        if (m_ctx->hud)
            m_ctx->hud->SetRank(xp.newRank);
    }

    // --- W5 onboarding (Task 4) reply handlers ---------------------------------
    // TFLoginFlow (Task 5) does not exist yet, so these stash into member state
    // (see the getters in TFClientNet.h) instead of forwarding to `m_ctx->
    // loginFlow`. Task 5/6 should replace the stash-and-log body with a direct
    // forward once that pointer is wired.

    void TFClientNet::OnLoginReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_AuthReply))
            return;
        TF_AuthReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        m_loggedIn = rep.ok != 0;
        m_accountId = rep.accountId;
        m_lastAuthErr = rep.err;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] login reply: ok=%d err=%u account=%llu", rep.ok,
                       static_cast<unsigned>(rep.err), static_cast<unsigned long long>(rep.accountId));
        if (m_ctx->loginFlow)
            m_ctx->loginFlow->OnLoginReply(rep.ok != 0, rep.err, rep.accountId);
    }

    void TFClientNet::OnRegisterReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_AuthReply))
            return;
        TF_AuthReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        m_lastAuthErr = rep.err;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] register reply: ok=%d err=%u account=%llu", rep.ok,
                       static_cast<unsigned>(rep.err), static_cast<unsigned long long>(rep.accountId));
        if (m_ctx->loginFlow)
            m_ctx->loginFlow->OnRegisterReply(rep.ok != 0, rep.err);
    }

    void TFClientNet::OnCharListReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_CharListReply))
            return;
        TF_CharListReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        m_charList.assign(rep.chars, rep.chars + std::min<uint8_t>(rep.count, 5));
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] char list reply: %u character(s)",
                       static_cast<unsigned>(rep.count));
        if (m_ctx->loginFlow)
            m_ctx->loginFlow->OnCharList(rep);
    }

    void TFClientNet::OnCharCreateReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_CharOpReply))
            return;
        TF_CharOpReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        m_lastCharOpErr = rep.err;
        m_lastCharOpId = rep.charId;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] char create reply: ok=%d err=%u charId=%llu", rep.ok,
                       static_cast<unsigned>(rep.err), static_cast<unsigned long long>(rep.charId));
        if (m_ctx->loginFlow)
            m_ctx->loginFlow->OnCharOpReply(rep.ok != 0, rep.err, rep.charId);
    }

    void TFClientNet::OnCharDeleteReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_CharOpReply))
            return;
        TF_CharOpReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        m_lastCharOpErr = rep.err;
        m_lastCharOpId = rep.charId;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] char delete reply: ok=%d err=%u charId=%llu", rep.ok,
                       static_cast<unsigned>(rep.err), static_cast<unsigned long long>(rep.charId));
        if (m_ctx->loginFlow)
            m_ctx->loginFlow->OnCharOpReply(rep.ok != 0, rep.err, rep.charId);
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
