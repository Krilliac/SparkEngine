/**
 * @file TFClientNetLoopback.cpp
 * @brief TFClientNet listen-host/standalone plumbing: the client-originated
 *        loopback router (no socket round-trip) and the HUD feedback the
 *        in-process authority player gets from the server event bus (same
 *        class, split per repo file-size rules — core pump/prediction logic
 *        lives in TFClientNet.cpp, the TFMsg handlers in
 *        TFClientNetHandlers.cpp and the view path in TFClientNetView.cpp).
 */
#include "Net/TFClientNet.h"

#include "Core/TFEvents.h"
#include "Core/TFTypes.h"
#include "Data/TFDataTables.h"
#include "Game/TFOutfitSystem.h" // Outfits lane: killfeed name tags
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFServerSim.h"
#include "UI/TFHUD.h"

#include "Utils/LogMacros.h"

#include <cstdio>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        void PlayerLabel(PlayerId id, char out[16])
        {
            if (id == kInvalidPlayer)
                std::snprintf(out, 16, "-");
            else if (id == kTFLocalHostPlayer)
                std::snprintf(out, 16, "HOST");
            else
                std::snprintf(out, 16, "P%u", id);
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Loopback routing (listen host / standalone — no socket round-trip)
    // ---------------------------------------------------------------------------

    void TFClientNet::RouteLoopback(TFMsg id, const void* payload, size_t size)
    {
        EnsureLocalHostIdentity();
        const PlayerId me = m_localPlayer;

        switch (id)
        {
#ifdef ENABLE_NETWORKING
        // W5 T6 (T4-review #1 security fix): every client-originated gameplay
        // AND onboarding message now routes straight into
        // TFServerSim::RouteClientMessage — the SAME dispatcher the socket
        // path uses (RegisterNetHandlers) — so the enter-world gate added
        // there (RouteClientMessage) applies identically to the listen-host/
        // standalone loopback player and to real network clients: the local
        // host (kTFLocalHostPlayer) must complete login -> character
        // select/create -> enter-world via TFLoginFlow exactly like a
        // networked client before it can move, spawn, fire, or switch
        // factions. RouteClientMessage only exists under ENABLE_NETWORKING
        // (mirrors every other Handle* on TFServerSim); the #else branch
        // below preserves the pre-W5 direct-call behavior for builds without
        // networking (no socket attack surface to close there).
        case TFMsg::ClientInput:
        case TFMsg::SpawnRequest:
        case TFMsg::FireEvent:
        case TFMsg::FactionSelect:
        case TFMsg::VehicleEnter:
        case TFMsg::VehicleExit:
        case TFMsg::AegisDeploy:
        case TFMsg::SquadMsg:
        case TFMsg::ChatMsg:
        case TFMsg::LoginRequest:
        case TFMsg::RegisterRequest:
        case TFMsg::CharListRequest:
        case TFMsg::CharCreateReq:
        case TFMsg::CharDeleteReq:
        case TFMsg::EnterWorldReq:
        case TFMsg::RedeployRequest: // W7 ui-map-keys: listen-host/standalone redeploy
            // final-review #3: vehicle/squad verbs now share the same
            // enter-world gate as the other gameplay ids on this path too.
            if (m_ctx->serverSim)
                m_ctx->serverSim->RouteClientMessage(me, id, payload, size);
            break;
#else
        case TFMsg::ClientInput:
            if (size == sizeof(TF_ClientInput) && m_ctx->serverSim)
            {
                TF_ClientInput in;
                std::memcpy(&in, payload, sizeof(in));
                m_ctx->serverSim->EnqueueInput(me, in);
            }
            break;

        case TFMsg::SpawnRequest:
            if (size == sizeof(TF_SpawnRequest) && m_ctx->players)
            {
                TF_SpawnRequest rq;
                std::memcpy(&rq, payload, sizeof(rq));
                m_ctx->players->ServerHandleSpawnRequest(me, rq);
            }
            break;

        case TFMsg::FireEvent:
            if (size == sizeof(TF_FireEvent) && m_ctx->weapons)
            {
                TF_FireEvent ev;
                std::memcpy(&ev, payload, sizeof(ev));
                m_ctx->weapons->ServerHandleFire(me, ev);
            }
            break;

        case TFMsg::FactionSelect:
            if (size == sizeof(TF_FactionSelect))
            {
                TF_FactionSelect sel;
                std::memcpy(&sel, payload, sizeof(sel));
                const auto f = static_cast<FactionId>(sel.faction);
                // Mirror the alive-guard TFServerSim::HandleFactionSelect enforces for
                // networked clients: no faction/team switch while this player has an
                // active pawn. Without this, the listen-host/standalone loopback path
                // could team-swap mid-life in a way real networked clients cannot.
                if (m_ctx->serverSim && m_ctx->serverSim->IsPlayerAlive(me))
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "[TF] player %u tried to switch faction while alive - ignored", me);
                    break;
                }
                if (m_ctx->serverSim)
                    m_ctx->serverSim->SetPlayerFaction(me, f);
                if (m_ctx->players)
                    m_ctx->players->ServerHandleFactionSelect(me, f);
                m_ctx->localFaction = f;
            }
            break;
#endif

        default:
            break; // LoadoutChange/Squad/Chat: TF-W2 server routing
        }
    }

    // ---------------------------------------------------------------------------
    // Local (bus) feedback for the in-process authority player
    // ---------------------------------------------------------------------------

    void TFClientNet::PushKillfeedEntry(PlayerId killer, PlayerId victim, WeaponId weapon, FactionId killerF,
                                        FactionId victimF, bool headshot)
    {
        if (!m_ctx->hud)
            return;
        char killerName[16], victimName[16];
        PlayerLabel(killer, killerName);
        PlayerLabel(victim, victimName);
        // Outfits lane: prepend "[TAG] " when the player is in an outfit.
        char killerTagged[26], victimTagged[26];
        OutfitTaggedLabel(m_ctx->outfits ? m_ctx->outfits->GetOutfitTag(killer) : "", killerName, killerTagged,
                          sizeof(killerTagged));
        OutfitTaggedLabel(m_ctx->outfits ? m_ctx->outfits->GetOutfitTag(victim) : "", victimName, victimTagged,
                          sizeof(victimTagged));

        const char* weaponName = "-";
        if (m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const WeaponDef* wd = m_ctx->data->GetWeapon(weapon))
                weaponName = wd->name.c_str();
        }
        // W6 combat HUD: extended overload — headshot marker + player ids for the
        // local-row highlight and the pure-client death panel.
        m_ctx->hud->PushKillfeed(killerTagged, weaponName, victimTagged, killerF, victimF, headshot, killer, victim);
    }

    void TFClientNet::OnBusPlayerKilled(const EvPlayerKilled& ev)
    {
        // Only the in-process authority player needs the bus mirror; connected
        // clients get TFMsg::KillEvent from the server instead.
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->HasLocalPlayer())
            return;

        FactionId killerF = FactionId::None;
        FactionId victimF = FactionId::None;
        if (m_ctx->players)
        {
            killerF = m_ctx->players->FactionOf(ev.killer);
            victimF = m_ctx->players->FactionOf(ev.victim);
        }
        PushKillfeedEntry(ev.killer, ev.victim, ev.weapon, killerF, victimF, ev.headshot);

        if (ev.killer == m_ctx->localPlayer && m_ctx->hud)
            m_ctx->hud->ShowHitmarker(true);
    }

    void TFClientNet::OnBusPlayerDamaged(const EvPlayerDamaged& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->HasLocalPlayer() || !m_ctx->hud || !m_ctx->players)
            return;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
            return;
        if (ev.attacker == pawn.entity && ev.victim != pawn.entity)
            m_ctx->hud->ShowHitmarker(false);
        if (ev.victim == pawn.entity)
            m_ctx->hud->ShowDamageFrom(0);
    }

} // namespace Terrafront
