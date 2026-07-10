/**
 * @file TFPlayerSystemClient.cpp
 * @brief TFPlayerSystem query surface + pure-client half: PawnInfo queries,
 *        replicated-pawn discovery (SyncClientRecords), placeholder pawn
 *        visuals, and the debug panel. Server spawn/kill/respawn logic lives
 *        in TFPlayerSystem.cpp (same class, split per repo file-size rules).
 */
#include "Game/TFPlayerSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFVisualUtils.h"
#include "Net/TFClientNet.h"
#include "Net/TFReplication.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <string>
#include <unordered_set>

namespace Terrafront {

namespace {

constexpr float kRadToDeg = 57.2957795f;

} // namespace

// ---------------------------------------------------------------- queries

bool TFPlayerSystem::FillPawnInfo(PlayerId player, const PlayerRec& rec, PawnInfo& out) const
{
    if (!rec.hasPawn)
        return false;

    out = PawnInfo{};
    out.entity  = rec.pawn;
    out.owner   = player;
    out.faction = rec.faction;
    out.cls     = rec.cls;
    out.alive   = rec.alive;

    if (m_ctx->IsAuthority())
    {
        // ECS truth: TFServerSim writes the Transform each fixed tick
        // (feet position, rotation.y = yaw degrees); health/shield live in
        // HealthComponent/TFShieldComp; velocity/pitch in TFPawnMoveComp.
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        const auto e = static_cast<EntityID>(rec.local);
        if (world && rec.local != 0 && world->GetRegistry().valid(e))
        {
            if (const Transform* t = world->GetComponent<Transform>(e))
            {
                out.pos[0] = t->position.x;
                out.pos[1] = t->position.y;
                out.pos[2] = t->position.z;
                out.yaw = t->rotation.y / kRadToDeg;
            }
            if (const TFPawnMoveComp* mv = world->GetComponent<TFPawnMoveComp>(e))
            {
                out.vel[0] = mv->vel[0];
                out.vel[1] = mv->vel[1];
                out.vel[2] = mv->vel[2];
                out.pitch = mv->pitch;
            }
            if (const HealthComponent* hc = world->GetComponent<HealthComponent>(e))
                out.health = hc->health;
            if (const TFShieldComp* sc = world->GetComponent<TFShieldComp>(e))
                out.shield = sc->cur;
        }
        return true;
    }

    // Pure client: replicated store, predicted override for the local player.
    if (m_ctx->replication)
    {
        RemotePawn rp{};
        if (m_ctx->replication->GetRemotePawn(rec.pawn, rp))
        {
            out.alive = rp.alive;
            out.pos[0] = rp.pos[0]; out.pos[1] = rp.pos[1]; out.pos[2] = rp.pos[2];
            out.vel[0] = rp.vel[0]; out.vel[1] = rp.vel[1]; out.vel[2] = rp.vel[2];
            out.yaw = rp.yaw;
            out.pitch = rp.pitch;
            out.health = rp.health;
            out.shield = rp.shield;
        }
    }
    if (player == m_ctx->localPlayer && m_ctx->clientNet)
    {
        float pos[3], vel[3], yaw, pitch;
        if (m_ctx->clientNet->GetPredictedLocalState(pos, vel, yaw, pitch))
        {
            out.pos[0] = pos[0]; out.pos[1] = pos[1]; out.pos[2] = pos[2];
            out.vel[0] = vel[0]; out.vel[1] = vel[1]; out.vel[2] = vel[2];
            out.yaw = yaw;
            out.pitch = pitch;
        }
    }
    return true;
}

bool TFPlayerSystem::GetPawnByEntity(EntityId entity, PawnInfo& out) const
{
    auto it = m_pawnOwner.find(entity);
    if (it == m_pawnOwner.end())
        return false;
    auto pit = m_players.find(it->second);
    return pit != m_players.end() && FillPawnInfo(pit->first, pit->second, out);
}

bool TFPlayerSystem::GetPawnByPlayer(PlayerId player, PawnInfo& out) const
{
    auto it = m_players.find(player);
    return it != m_players.end() && FillPawnInfo(player, it->second, out);
}

void TFPlayerSystem::ForEachAlivePawn(const std::function<void(const PawnInfo&)>& fn) const
{
    for (const auto& [player, rec] : m_players)
    {
        PawnInfo info;
        if (rec.alive && FillPawnInfo(player, rec, info))
            fn(info);
    }
}

FactionId TFPlayerSystem::FactionOf(PlayerId player) const
{
    auto it = m_players.find(player);
    return it == m_players.end() ? FactionId::None : it->second.faction;
}

bool TFPlayerSystem::ResolveEntity(EntityId netEntity, uint32_t& outLocalEntity) const
{
    auto it = m_pawnOwner.find(netEntity);
    if (it == m_pawnOwner.end())
        return false;
    auto pit = m_players.find(it->second);
    if (pit == m_players.end() || !pit->second.hasPawn || pit->second.local == 0)
        return false;
    outLocalEntity = pit->second.local;
    return true;
}

// ---------------------------------------------------------------- client ops

void TFPlayerSystem::ClientOnSpawnReply(const TF_SpawnReply& reply)
{
    if (!m_ctx || m_ctx->IsAuthority() || !reply.accepted)
        return;
    m_pendingLocalPawn = reply.entityId;
    // SyncClientRecords associates the record once the pawn replicates in.
}

void TFPlayerSystem::SyncClientRecords()
{
    if (!m_ctx->replication)
        return;
    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;

    std::unordered_set<EntityId> seen;
    m_ctx->replication->ForEachRemotePawn([&](const RemotePawn& rp) {
        seen.insert(rp.entity);

        PlayerRec& rec = m_players[rp.owner];
        if (!rec.hasPawn || rec.pawn != rp.entity)
        {
            if (rec.hasPawn)
                DestroyPawn(rec); // respawn under a fresh entity id

            rec.pawn = rp.entity;
            rec.local = 0;
            rec.hasPawn = true;
            m_pawnOwner[rp.entity] = rp.owner;

            if (world)
            {
                EntityID e = world->CreateEntity("TF_Remote_P" + std::to_string(rp.owner));
                if (static_cast<uint32_t>(e) == 0)
                    e = world->CreateEntity("TF_Remote_P" + std::to_string(rp.owner));
                Transform& t = world->AddComponent<Transform>(e);
                t.position = {rp.pos[0], rp.pos[1], rp.pos[2]};
                t.rotation.y = rp.yaw * kRadToDeg;

                NetworkIdentity& ni = world->AddComponent<NetworkIdentity>(e);
                ni.networkID = rp.entity;
                ni.ownerClientID = rp.owner;
                ni.isLocalAuthority = false; // server owns all pawn state

                world->AddComponent<TFFactionComp>(e).faction = rp.faction;
                world->AddComponent<TFClassComp>(e).cls = rp.cls;
                world->AddComponent<TFPawnComp>(e).owner = rp.owner;

                rec.local = static_cast<uint32_t>(e);
                if (rp.owner != m_ctx->localPlayer) // own pawn is first-person
                    AttachPawnVisual(rec.local, rp.faction, rp.cls);
            }

            if (rp.owner == m_ctx->localPlayer && m_pendingLocalPawn == rp.entity)
                m_pendingLocalPawn = 0;
        }

        rec.faction = rp.faction;
        rec.cls     = rp.cls;
        rec.alive   = rp.alive;
    });

    // Pawns that left the replication store (destroyed server-side).
    for (auto& [player, rec] : m_players)
    {
        if (rec.hasPawn && !seen.contains(rec.pawn))
            DestroyPawn(rec);
    }
}

// ---------------------------------------------------------------- visuals

void TFPlayerSystem::AttachPawnVisual(uint32_t localEntity, FactionId faction, ClassId cls)
{
    World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
    const auto e = static_cast<EntityID>(localEntity);
    if (!world || !world->GetRegistry().valid(e) || world->HasComponent<MeshRenderer>(e))
        return;

    // Humanoid soldier proxy (Kenney mini character, CC0) — pre-scaled at
    // import to ~1.6 m with feet at y=0, so it stands on the pawn's feet
    // position at Transform scale 1. Faction is telegraphed by the material
    // tint; the model's own colormap texture supplies the body detail. Mesh
    // path is data-driven (Assets/MMOFPS/Data/presentation.json pawnMesh);
    // the literal here is only the byte-identical fallback when no
    // TFDataTables instance exists at all (e.g. some headless harnesses).
    // Per-class body when classes.json gives one (Ghost/Striker/Medtech/
    // Fabricator/Bulwark/Colossus silhouettes); otherwise the shared
    // presentation.pawnMesh; literal only when no TFDataTables exists at all.
    std::string pawnMesh = "Models/MMOFPS/characters/soldier.obj";
    if (m_ctx->data)
    {
        pawnMesh = m_ctx->data->GetPresentation().pawnMesh;
        if (const ClassDef* cd = m_ctx->data->GetClass(cls); cd && !cd->mesh.empty())
            pawnMesh = cd->mesh;
    }
    MeshRenderer& mr = world->AddComponent<MeshRenderer>(e);
    mr.meshPath = "Assets/" + pawnMesh;
    mr.materialPath = FactionStructureMaterial(*m_ctx, faction);
    mr.castShadows = true;

    if (Transform* t = world->GetComponent<Transform>(e))
        t->scale = {1.0f, 1.0f, 1.0f};
}

// ---------------------------------------------------------------- debug UI

void TFPlayerSystem::RenderDebugUI()
{
#ifdef SPARK_HAS_IMGUI
    if (!ImGui::CollapsingHeader("TF Players"))
        return;
    ImGui::Text("records: %zu  (pawn map %zu, pending local %u)",
                m_players.size(), m_pawnOwner.size(), m_pendingLocalPawn);
    for (const auto& [player, rec] : m_players)
    {
        PawnInfo p{};
        if (FillPawnInfo(player, rec, p))
        {
            ImGui::Text("p%u %s %s pawn=%u pos=(%.1f %.1f %.1f) hp=%.0f+%.0f", player,
                        FactionTag(p.faction), p.alive ? "alive" : "dead", p.entity,
                        p.pos[0], p.pos[1], p.pos[2], p.health, p.shield);
        }
        else
        {
            ImGui::Text("p%u %s (no pawn)", player, FactionTag(rec.faction));
        }
    }
#endif
}

} // namespace Terrafront
