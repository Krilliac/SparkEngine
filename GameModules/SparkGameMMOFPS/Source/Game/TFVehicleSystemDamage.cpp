/**
 * @file TFVehicleSystemDamage.cpp
 * @brief W3 vehicles — damage/destruction: the server-authoritative hp pool
 *        (friendly fire 50%), radial splash, destruction with occupant eject
 *        + explosion splash + XP award, and the W13 persistent wrecks. Split
 *        from TFVehicleSystem.cpp; shared internals live in
 *        TFVehicleSystemInternal.h.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVehiclePhysics.h"
#include "Game/TFVehicleSystemInternal.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace Terrafront
{

    using namespace VehicleDetail;

    namespace
    {

        constexpr uint8_t kDamageKindExplosive = 1; // TFNetProtocol damageKind convention

        // W13 persistent wreck (kills leave a mark).
        constexpr float kTFVehWreckLifeSec = 15.0f;
        constexpr char kTFVehWreckMaterial[] = "Assets/Materials/MMOFPS/Structure_AlloyDark.json"; // charred tint

    } // namespace

    // ---------------------------------------------------------------------------
    // Damage / destruction
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ServerDamageVehicle(EntityId vehicle, float amount, EntityId attackerPawn,
                                              PlayerId attackerPlayer, WeaponId weapon)
    {
        (void)weapon; // kept for kill-feed symmetry with ServerApplyDamage (TF-W4)
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
            return;
        VehicleRec* v = FindRec(vehicle);
        if (!v || v->hp <= 0.0f)
            return;

        // Friendly fire mirrors infantry policy (50%, DESIGN §4).
        const FactionId attackerFaction = (m_ctx->players && attackerPlayer != kInvalidPlayer)
                                              ? m_ctx->players->FactionOf(attackerPlayer)
                                              : FactionId::None;
        const bool friendly = attackerFaction != FactionId::None && attackerFaction == v->faction;
        if (friendly)
            amount *= 0.5f;

        v->hp = std::max(0.0f, v->hp - amount);
        if (!friendly && attackerPlayer != kInvalidPlayer)
            v->lastAttacker = attackerPlayer;

        const bool killed = v->hp <= 0.0f;

        // Attacker hitmarker: network clients get TF_HitConfirm, the in-process
        // authority player gets it via the EvPlayerDamaged bus mirror (the same
        // split TFDamageSystem uses).
#ifdef ENABLE_NETWORKING
        if (attackerPlayer != kInvalidPlayer && m_ctx->role != NetRole::Standalone)
        {
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.IsInitialized())
            {
                TF_HitConfirm hc{};
                hc.victimEntity = vehicle;
                hc.damage = static_cast<uint16_t>(std::min(amount, 65535.0f));
                hc.headshot = 0;
                hc.killed = killed ? 1 : 0;
                Spark::Net::NetworkMessage msg;
                msg.type = static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(TFMsg::HitConfirm));
                msg.channel = Spark::Net::ChannelType::Reliable;
                msg.payload.resize(sizeof(hc));
                std::memcpy(msg.payload.data(), &hc, sizeof(hc));
                nm.SendToClient(attackerPlayer, msg);
            }
        }
#endif
        if (m_events)
            m_events->Fire(EvPlayerDamaged{vehicle, attackerPawn, amount, kDamageKindExplosive});

        if (killed)
            DestroyVehicle(*v, v->lastAttacker != kInvalidPlayer ? v->lastAttacker : attackerPlayer);
    }

    void TFVehicleSystem::ServerApplySplash(const float at[3], float radiusM, float damage, float vsVehicleMult,
                                            EntityId attackerPawn, PlayerId attackerPlayer, WeaponId weapon,
                                            EntityId excludeVehicle)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || radiusM <= 0.0f || damage <= 0.0f)
            return;

        // Collect first, damage second: ServerDamageVehicle can destroy (erase)
        // records, which would invalidate iteration over m_vehicles.
        struct Hit
        {
            EntityId veh;
            float dmg;
        };
        std::vector<Hit> hits;
        for (const VehicleRec& v : m_vehicles)
        {
            if (v.hp <= 0.0f || v.entity == excludeVehicle)
                continue;
            const float d = std::sqrt(Dist2XZ(v.pos, at[0], at[2]) + (v.pos[1] - at[1]) * (v.pos[1] - at[1]));
            const float reach = radiusM + VehicleRadius(v.vehId);
            if (d >= reach)
                continue;
            const float dmg = damage * (1.0f - d / reach) * vsVehicleMult;
            if (dmg > 1.0f)
                hits.push_back({v.entity, dmg});
        }
        for (const Hit& h : hits)
            ServerDamageVehicle(h.veh, h.dmg, attackerPawn, attackerPlayer, weapon);
    }

    void TFVehicleSystem::DestroyVehicle(VehicleRec& v, PlayerId destroyer)
    {
        const VehicleDef* def = DefOf(v.vehId);
        const EntityId entity = v.entity;
        const VehicleId kind = v.vehId;
        const FactionId faction = v.faction;
        ++m_destroyed;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] vehicle %u (%s) destroyed by player %u", entity,
                       def ? def->name.c_str() : "?", destroyer);

        // 1) Eject occupants ALIVE with 50% max-pool damage (DESIGN W3: no
        //    guaranteed occupant kill). Excluded from this vehicle's death splash.
        std::vector<EntityId> occupantPawns;
        for (uint8_t i = 0; i < v.seatCount; ++i)
        {
            const PlayerId p = v.seats[i];
            if (p == kInvalidPlayer)
                continue;
            PawnInfo pawn{};
            const bool havePawn = m_ctx->players && m_ctx->players->GetPawnByPlayer(p, pawn);
            UnseatPlayer(p, true); // exit latch places them beside the wreck
            if (havePawn)
            {
                occupantPawns.push_back(pawn.entity);
                // 50% of THIS OCCUPANT'S max health+shield pool (DESIGN §4); survivable
                // when healthy, harsh when already hurt -- but never a guaranteed kill.
                // Must be scaled per-class: a flat 1000 assumes the standard 500/500
                // infantry pool and massively under-punishes a Colossus (2200/0).
                float maxPool = 1000.0f; // fallback: standard 500 health + 500 shield
                if (m_ctx->data && m_ctx->data->IsLoaded())
                {
                    if (const ClassDef* cd = m_ctx->data->GetClass(pawn.cls))
                        maxPool = cd->health + cd->shield;
                }
                if (m_ctx->damage)
                    m_ctx->damage->ServerApplyDamage(pawn.entity, entity, kInvalidPlayer,
                                                     kTFVehEjectDamageFrac * maxPool, kDamageKindExplosive,
                                                     kInvalidWeapon, false);
            }
        }

        // 2) Explosion splash on other nearby pawns (kill credit -> destroyer).
        if (m_ctx->players && m_ctx->damage)
        {
            const float at[3] = {v.pos[0], v.pos[1] + 1.0f, v.pos[2]};
            std::vector<std::pair<EntityId, float>> hits;
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& pawn)
                {
                    for (EntityId occ : occupantPawns)
                        if (pawn.entity == occ)
                            return;
                    const float dx = pawn.pos[0] - at[0];
                    const float dy = pawn.pos[1] - at[1];
                    const float dz = pawn.pos[2] - at[2];
                    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (d >= kTFVehExplodeRadiusM)
                        return;
                    const float dmg = kTFVehExplodeDamage * (1.0f - d / kTFVehExplodeRadiusM);
                    if (dmg > 1.0f)
                        hits.emplace_back(pawn.entity, dmg);
                });
            for (const auto& [pawnEntity, dmg] : hits)
                m_ctx->damage->ServerApplyDamage(pawnEntity, entity, destroyer, dmg, kDamageKindExplosive,
                                                 kInvalidWeapon, false);
        }

        // 3) XP to the destroyer (enemy kills only).
        if (m_ctx->progression && destroyer != kInvalidPlayer && m_ctx->players &&
            m_ctx->players->FactionOf(destroyer) != faction)
            m_ctx->progression->ServerAwardXP(destroyer, kTFVehKillXP, kXPReasonKill);

        // 4) Feedback + replication destroy + entity teardown.
        if (def)
            PlayOneShot(def->explodeAudio);
        if (m_events)
            m_events->Fire(EvVehicleDestroyed{entity, kind, destroyer});
#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            ServerSendDestroy(entity);
#endif

        if (m_joltDrive)
            m_joltDrive->DetachVehicle(entity);

        // W8: rig children first — the barrel is a GRANDCHILD (turret-parented),
        // which the direct hull child-sweep below would miss.
        DestroyTurretRig(v.rig);

        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (world && v.local != 0)
        {
            const auto e = static_cast<EntityID>(v.local);
            // Destroy any hull-parented children (turret) first so the render
            // pass never walks a dangling parent chain after the hull is gone.
            auto& registry = world->GetRegistry();
            std::vector<EntityID> children;
            for (auto child : world->GetEntitiesWith<Transform>())
            {
                if (registry.get<Transform>(child).parent == e)
                    children.push_back(child);
            }
            for (auto child : children)
                world->DestroyEntity(child);
            // W13: leave a charred, static wreck in place instead of an
            // immediate despawn -- kills leave a mark. This vehicle is removed
            // from m_vehicles/m_lastSent below, so ServerDamageVehicle,
            // RaycastVehicles, ForEachVehicle and TFGroundFx/TFVehicleFx never
            // see it again; UpdateWrecks() cleans up the leftover entity after
            // kTFVehWreckLifeSec.
            SpawnWreck(v.local);
        }
        m_lastSent.erase(entity);
        m_lastAimSent.erase(entity);
        m_vehicles.erase(std::remove_if(m_vehicles.begin(), m_vehicles.end(),
                                        [entity](const VehicleRec& r) { return r.entity == entity; }),
                         m_vehicles.end());
    }

    // ---------------------------------------------------------------------------
    // W13 persistent wreck (kills leave a mark)
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::SpawnWreck(uint32_t local)
    {
        if (local == 0)
            return;
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return;
        const auto e = static_cast<EntityID>(local);
        if (!world->GetRegistry().valid(e))
            return;
        if (MeshRenderer* mr = world->GetComponent<MeshRenderer>(e))
            mr->materialPath = kTFVehWreckMaterial; // charred/darkened tint, same mesh
        m_wrecks.push_back({local, m_clock + kTFVehWreckLifeSec});
    }

    void TFVehicleSystem::UpdateWrecks()
    {
        if (m_wrecks.empty())
            return;
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        for (auto it = m_wrecks.begin(); it != m_wrecks.end();)
        {
            if (m_clock < it->expireAt)
            {
                ++it;
                continue;
            }
            if (world && it->local != 0)
            {
                const auto e = static_cast<EntityID>(it->local);
                if (world->GetRegistry().valid(e))
                    world->DestroyEntity(e);
            }
            it = m_wrecks.erase(it);
        }
    }

} // namespace Terrafront
