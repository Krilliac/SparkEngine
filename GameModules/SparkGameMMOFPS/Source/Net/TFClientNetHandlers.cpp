/**
 * @file TFClientNetHandlers.cpp
 * @brief TFClientNet message + view plumbing: client-side TFMsg handlers,
 *        the listen-host/standalone loopback router, HUD feedback from the
 *        server event bus, remote-pawn interpolation, the first-person
 *        camera and the debug panel. Core pump/prediction logic lives in
 *        TFClientNet.cpp (same class, split per repo file-size rules —
 *        mirrors the TFWeaponSystem/TFWeaponServer split).
 */
#include "Net/TFClientNet.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponMath.h"
#include "Game/TFWeaponSystem.h"
#include "Net/TFReplication.h"
#include "Net/TFServerSim.h"
#include "UI/TFHUD.h"

#include "Camera/SparkEngineCamera.h"
#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Terrafront {

namespace {

constexpr float  kInterpDelaySec = 0.100f;  // remote pawn render delay
constexpr float  kRadToDeg       = 57.2957795f;
constexpr double kTwoPi          = 6.28318530717958647692;

void PlayerLabel(PlayerId id, char out[16])
{
    if (id == kInvalidPlayer)
        std::snprintf(out, 16, "-");
    else if (id == kTFLocalHostPlayer)
        std::snprintf(out, 16, "HOST");
    else
        std::snprintf(out, 16, "P%u", id);
}

/// Shift `yaw` by whole turns so it lands nearest to `reference` (keeps the
/// interpolation buffer free of ±pi wrap pops).
float UnwrapNear(float reference, float yaw)
{
    const double turns = std::round((static_cast<double>(reference) - yaw) / kTwoPi);
    return yaw + static_cast<float>(turns * kTwoPi);
}

} // namespace

// ---------------------------------------------------------------------------
// Remote pawn interpolation (pure client)
// ---------------------------------------------------------------------------

void TFClientNet::UpdateRemotePawns()
{
    if (!m_ctx->replication || !m_ctx->players)
        return;

    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
    const float renderTime = static_cast<float>(m_clock) - kInterpDelaySec;

    std::unordered_map<EntityId, InterpEntry>& interp = m_interp;
    std::vector<EntityId> seen;
    seen.reserve(interp.size() + 4);

    m_ctx->replication->ForEachRemotePawn([&](const RemotePawn& rp) {
        seen.push_back(rp.entity);
        if (rp.owner == m_ctx->localPlayer)
            return; // own pawn: first-person, prediction drives the camera

        InterpEntry& e = interp[rp.entity];
        if (!e.has || rp.recvTime != e.lastRecvTime)
        {
            // New replication sample: timestamp on OUR clock so evaluation and
            // sampling share one time base regardless of sender cadence.
            const float y = e.has ? UnwrapNear(e.lastYaw, rp.yaw) : rp.yaw;
            e.pos.AddSample({rp.pos[0], rp.pos[1], rp.pos[2]}, static_cast<float>(m_clock));
            e.yaw.AddSample(y, static_cast<float>(m_clock));
            e.lastYaw = y;
            e.lastRecvTime = rp.recvTime;
            e.has = true;
        }

        if (!world || !e.pos.HasSamples())
            return;
        uint32_t localEnt = 0;
        if (!m_ctx->players->ResolveEntity(rp.entity, localEnt))
            return; // visual entity not created yet (TFPlayerSystem::SyncClientRecords)
        const auto ecsEnt = static_cast<EntityID>(localEnt);
        if (!world->GetRegistry().valid(ecsEnt))
            return;
        if (Transform* t = world->GetComponent<Transform>(ecsEnt))
        {
            const DirectX::XMFLOAT3 p = e.pos.Evaluate(renderTime);
            t->position = p;
            t->rotation.y = e.yaw.Evaluate(renderTime) * kRadToDeg;
        }
    });

    // Drop buffers for entities that left the replication store.
    for (auto it = interp.begin(); it != interp.end();)
    {
        if (std::find(seen.begin(), seen.end(), it->first) == seen.end())
            it = interp.erase(it);
        else
            ++it;
    }
}

// ---------------------------------------------------------------------------
// First-person camera
// ---------------------------------------------------------------------------

void TFClientNet::DriveFirstPersonCamera(bool aliveLocalPawn, const float feetPos[3])
{
    if (!aliveLocalPawn)
        return;
    SparkEngineCamera* cam = m_ctx->engine ? m_ctx->engine->GetCamera() : nullptr;
    if (!cam)
        return;

    float eye[3] = {feetPos[0], feetPos[1], feetPos[2]};
    if (!m_ctx->IsAuthority() && m_predActive)
    {
        eye[0] = m_predState.position.x;
        eye[1] = m_predState.position.y;
        eye[2] = m_predState.position.z;
    }
    // Authority roles read the pawn Transform truth (TFServerSim writes it).

    cam->SetPosition({eye[0], eye[1] + WeaponMath::kEyeHeightM, eye[2]});
}

// ---------------------------------------------------------------------------
// Loopback routing (listen host / standalone — no socket round-trip)
// ---------------------------------------------------------------------------

void TFClientNet::RouteLoopback(TFMsg id, const void* payload, size_t size)
{
    EnsureLocalHostIdentity();
    const PlayerId me = m_localPlayer;

    switch (id)
    {
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
                if (m_ctx->serverSim)
                    m_ctx->serverSim->SetPlayerFaction(me, f);
                if (m_ctx->players)
                    m_ctx->players->ServerHandleFactionSelect(me, f);
                m_ctx->localFaction = f;
            }
            break;

        default:
            break; // LoadoutChange/Squad/Chat: TF-W2 server routing
    }
}

// ---------------------------------------------------------------------------
// Local (bus) feedback for the in-process authority player
// ---------------------------------------------------------------------------

void TFClientNet::PushKillfeedEntry(PlayerId killer, PlayerId victim, WeaponId weapon,
                                    FactionId killerF, FactionId victimF)
{
    if (!m_ctx->hud)
        return;
    char killerName[16], victimName[16];
    PlayerLabel(killer, killerName);
    PlayerLabel(victim, victimName);

    const char* weaponName = "-";
    if (m_ctx->data && m_ctx->data->IsLoaded())
    {
        if (const WeaponDef* wd = m_ctx->data->GetWeapon(weapon))
            weaponName = wd->name.c_str();
    }
    m_ctx->hud->PushKillfeed(killerName, weaponName, victimName, killerF, victimF);
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
    PushKillfeedEntry(ev.killer, ev.victim, ev.weapon, killerF, victimF);

    if (ev.killer == m_ctx->localPlayer && m_ctx->hud)
        m_ctx->hud->ShowHitmarker(true);
}

void TFClientNet::OnBusPlayerDamaged(const EvPlayerDamaged& ev)
{
    if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->HasLocalPlayer() ||
        !m_ctx->hud || !m_ctx->players)
        return;

    PawnInfo pawn{};
    if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn))
        return;
    if (ev.attacker == pawn.entity && ev.victim != pawn.entity)
        m_ctx->hud->ShowHitmarker(false);
    if (ev.victim == pawn.entity)
        m_ctx->hud->ShowDamageFrom(0);
}

// ---------------------------------------------------------------------------
// Client-side TFMsg handlers
// ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

void TFClientNet::RegisterClientHandlers()
{
    using Spark::Net::MessageType;
    using Spark::Net::NetworkMessage;
    auto& nm = Spark::Net::NetworkManager::GetInstance();

    auto route = [&nm](TFMsg id, auto&& fn) {
        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                           std::forward<decltype(fn)>(fn));
    };

    route(TFMsg::WorldWelcome, [this](const NetworkMessage& m) {
        OnWorldWelcome(m.payload.data(), m.payload.size());
    });
    route(TFMsg::SpawnReply, [this](const NetworkMessage& m) {
        OnSpawnReply(m.payload.data(), m.payload.size());
    });
    route(TFMsg::HitConfirm, [this](const NetworkMessage& m) {
        OnHitConfirm(m.payload.data(), m.payload.size());
    });
    route(TFMsg::DamageEvent, [this](const NetworkMessage& m) {
        OnDamageEvent(m.payload.data(), m.payload.size());
    });
    route(TFMsg::KillEvent, [this](const NetworkMessage& m) {
        OnKillEvent(m.payload.data(), m.payload.size());
    });
    route(TFMsg::XPEvent, [this](const NetworkMessage& m) {
        OnXPEvent(m.payload.data(), m.payload.size());
    });

    // Accepted-but-unrouted W2 broadcasts (no "unknown message" warnings):
    for (TFMsg id : {TFMsg::RegionState, TFMsg::CaptureTick, TFMsg::ChatMsg, TFMsg::SquadMsg})
        route(id, [](const NetworkMessage&) {});

    m_handlersRegistered = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] client TFMsg handlers registered");
}

void TFClientNet::ReleaseClientHandlers()
{
    // NetworkManager has no per-type removal; replace our handlers with no-ops
    // so no dangling `this` survives module shutdown (TFServerSim pattern).
    using Spark::Net::MessageType;
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    for (TFMsg id : {TFMsg::WorldWelcome, TFMsg::SpawnReply, TFMsg::HitConfirm,
                     TFMsg::DamageEvent, TFMsg::KillEvent, TFMsg::XPEvent,
                     TFMsg::RegionState, TFMsg::CaptureTick, TFMsg::ChatMsg, TFMsg::SquadMsg})
    {
        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                           [](const Spark::Net::NetworkMessage&) {});
    }
    m_handlersRegistered = false;
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

    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] WorldWelcome: player %u, faction %s, %u regions, server t=%ums",
                   w.yourPlayerId, FactionTag(static_cast<FactionId>(w.yourFaction)),
                   static_cast<unsigned>(w.regionCount), w.serverTimeMs);

    // Faction picked before the connection existed: assert it now.
    if (w.yourFaction == 0 && m_ctx->localFaction != FactionId::None)
    {
        TF_FactionSelect sel{};
        sel.faction = static_cast<uint8_t>(m_ctx->localFaction);
        SendMsg(TFMsg::FactionSelect, &sel, sizeof(sel));
    }
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
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] spawn accepted: entity %u at (%.0f %.0f %.0f)",
                       rep.entityId, rep.posX, rep.posY, rep.posZ);
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
        const EntityId victim =
            m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) ? pawn.entity : 0;
        m_events->Fire(EvPlayerDamaged{victim, de.attackerEntity,
                                       static_cast<float>(de.damage), de.damageKind});
    }
}

void TFClientNet::OnKillEvent(const void* data, size_t size)
{
    if (size != sizeof(TF_KillEvent))
        return;
    TF_KillEvent ke;
    std::memcpy(&ke, data, sizeof(ke));

    PushKillfeedEntry(ke.killerPlayer, ke.victimPlayer, ke.weaponId,
                      static_cast<FactionId>(ke.killerFaction),
                      static_cast<FactionId>(ke.victimFaction));

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

#endif // ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Debug UI
// ---------------------------------------------------------------------------

void TFClientNet::RenderDebugUI()
{
#ifdef ENABLE_EDITOR
    if (!m_showDebug)
        return;
    if (ImGui::Begin("TF Client Net", &m_showDebug))
    {
        const char* mode = m_connected ? "remote client"
                          : (LocalLoopback() ? "local loopback" : "idle");
        ImGui::Text("mode        : %s", mode);
        ImGui::Text("local player: 0x%08X", m_localPlayer);
        ImGui::Text("inputs sent : %u (seq %u)", m_inputsSent,
                    m_ctx && !m_ctx->IsAuthority() ? m_prediction.GetCurrentSequence() : m_inputSeq);
        ImGui::Text("view        : yaw %.2f pitch %.2f rad", m_viewYaw, m_viewPitch);
        if (m_ctx && !m_ctx->IsAuthority())
        {
            ImGui::Separator();
            ImGui::Text("prediction  : %s, pending %zu, correction %.3fm, reconciles %u",
                        m_predActive ? "active" : "off",
                        m_prediction.GetPendingInputCount(),
                        m_prediction.GetLastCorrectionMagnitude(), m_reconciles);
            ImGui::Text("pred pos    : (%.1f %.1f %.1f) %s",
                        m_predState.position.x, m_predState.position.y, m_predState.position.z,
                        m_predState.isGrounded ? "ground" : "air");
            ImGui::Text("interp pawns: %zu", m_interp.size());
        }
        ImGui::Text("rank        : %u (xp %u)", m_lastRank, m_lastXPTotal);
    }
    ImGui::End();
#endif
}

} // namespace Terrafront
