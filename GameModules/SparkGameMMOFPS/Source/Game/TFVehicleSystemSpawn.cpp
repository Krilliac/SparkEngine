/**
 * @file TFVehicleSystemSpawn.cpp
 * @brief W3 vehicles — validated terminal purchase + vehicle entity creation,
 *        the W8 turret aim rig (attach / pose / teardown, shared by the
 *        server entity and client mirror paths) and the seat-weapon /
 *        muzzle-frame / raycast weapons integration. Split from
 *        TFVehicleSystem.cpp; shared internals live in
 *        TFVehicleSystemInternal.h.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVehiclePhysics.h"
#include "Game/TFVehicleSystemInternal.h"
#include "Game/TFVisualUtils.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    using namespace VehicleDetail;

    namespace
    {

        constexpr float kMapCenter = 2048.0f;
        constexpr int kMaxVehicles = 64;

        // --- W8 turret aim: per-vehicle rig mounts -----------------------------
        // Mesh-footprint constants like VehicleRadius, NOT balance data. Pivots
        // come from Tools/assetgen specs: the Ravager 90 mm mantlet+barrel
        // attaches at turret-space (0, 0.18, 0.75) on the tank_turret pivot; the
        // Aegis PDW pedestal sits on the apc roof ring at hull-space
        // (0, 2.10, 1.10) with the twin-barrel head on top of it.
        struct TurretRigSpec
        {
            VehicleId veh;
            const char* pitchMesh; ///< Assets-relative barrel/head OBJ
            float pitchPivot[3];   ///< yaw-parent space (headYawsToo: hull space)
            const char* baseMesh;  ///< optional static pedestal (nullptr = none)
            float basePivot[3];    ///< hull space
            bool headYawsToo;      ///< pitch child also carries yaw (no yaw mesh)
            float muzzleM;         ///< muzzle distance along aim dir from pitchPivot
        };
        constexpr TurretRigSpec kTurretRigs[] = {
            {VehicleId::Ravager,
             "Models/MMOFPS/weapons/veh_ravager_90.obj",
             {0.0f, 0.18f, 0.75f},
             nullptr,
             {0.0f, 0.0f, 0.0f},
             false,
             2.85f},
            {VehicleId::Aegis,
             "Models/MMOFPS/weapons/veh_aegis_pdw_head.obj",
             {0.0f, 2.40f, 1.10f},
             "Models/MMOFPS/weapons/veh_aegis_pdw_base.obj",
             {0.0f, 2.10f, 1.10f},
             true,
             0.95f},
        };

        const TurretRigSpec* RigSpecOf(VehicleId id)
        {
            for (const TurretRigSpec& r : kTurretRigs)
                if (r.veh == id)
                    return &r;
            return nullptr;
        }

        /// Rotate a local-space offset by a yaw (TF basis: forward = (sin, 0, cos)).
        void YawRotate(const float local[3], float yaw, float out[3])
        {
            const float c = std::cos(yaw);
            const float s = std::sin(yaw);
            out[0] = local[0] * c + local[2] * s;
            out[1] = local[1];
            out[2] = -local[0] * s + local[2] * c;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Purchase
    // ---------------------------------------------------------------------------

    uint32_t TFVehicleSystem::CreateVehicleEntity(const VehicleDef& def, FactionId faction, const float pos[3],
                                                  float yaw, TurretRig& outRig)
    {
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return 0;

        EntityID e = world->CreateEntity("TF_Veh_" + def.name);
        if (static_cast<uint32_t>(e) == 0)
            e = world->CreateEntity("TF_Veh_" + def.name); // id 0 is "no entity" in TF contracts

        Transform& t = world->AddComponent<Transform>(e);
        t.position = {pos[0], pos[1], pos[2]};
        t.rotation.y = yaw * kRadToDeg;

        HealthComponent& hc = world->AddComponent<HealthComponent>(e);
        hc.health = def.health;
        hc.maxHealth = def.health;

        world->AddComponent<TFFactionComp>(e).faction = faction;
        TFVehicleComp& vc = world->AddComponent<TFVehicleComp>(e);
        vc.vehId = def.id;
        world->AddComponent<TFAegisDeployComp>(e).active = false;

        // Faction-tinted OBJ visual (same tint materials as pawns; vehicles.json
        // model paths are Assets-relative).
        if (!def.model.empty())
        {
            MeshRenderer& mr = world->AddComponent<MeshRenderer>(e);
            mr.meshPath = "Assets/" + def.model;
            mr.materialPath = FactionStructureMaterial(*m_ctx, faction);
            mr.castShadows = true;
        }

        // W8: turret + barrel/head aim-rig children (seat-driven aim; the ECS
        // render pass uses the hierarchical GetWorldMatrix, so children follow
        // the hull's pose and add their own aim rotation on top).
        AttachTurretRig(static_cast<uint32_t>(e), def, faction, outRig);
        return static_cast<uint32_t>(e);
    }

    // ---------------------------------------------------------------------------
    // W8 turret aim rig (shared by the server entity + client mirror paths)
    // ---------------------------------------------------------------------------

    int TFVehicleSystem::TurretControllerSeat(const VehicleDef* def)
    {
        if (!def)
            return -1;
        const size_t count = std::min<size_t>(def->seats.size(), 8);
        for (size_t i = 0; i < count; ++i)
            if (!def->seats[i].weaponKey.empty())
                return static_cast<int>(i);
        return -1;
    }

    void TFVehicleSystem::AttachTurretRig(uint32_t hullLocal, const VehicleDef& def, FactionId faction, TurretRig& out)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || hullLocal == 0)
            return;
        const auto hull = static_cast<EntityID>(hullLocal);

        const auto makeChild = [&](const char* name, EntityID parent, const float pivot[3],
                                   const std::string& meshAssetsRel) -> uint32_t
        {
            const auto child = world->CreateEntity(name);
            Transform& ct = world->AddComponent<Transform>(child);
            ct.parent = parent;
            ct.position = {pivot[0], pivot[1], pivot[2]};
            MeshRenderer& cmr = world->AddComponent<MeshRenderer>(child);
            cmr.meshPath = "Assets/" + meshAssetsRel;
            cmr.materialPath = FactionStructureMaterial(*m_ctx, faction);
            cmr.castShadows = true;
            return static_cast<uint32_t>(child);
        };

        // Data-driven turret mesh (Ravager tank_turret): the yaw part.
        if (!def.turretMesh.empty())
            out.yawChild = makeChild("TF_VehTurret", hull, def.turretPivot, def.turretMesh);

        const TurretRigSpec* spec = RigSpecOf(def.id);
        if (!spec)
            return;

        if (spec->baseMesh)
            out.baseChild = makeChild("TF_VehTurretBase", hull, spec->basePivot, spec->baseMesh);

        if (spec->headYawsToo)
        {
            // Aegis: one head child on the hull carries yaw AND pitch.
            out.pitchChild = makeChild("TF_VehTurretHead", hull, spec->pitchPivot, spec->pitchMesh);
            out.yawChild = out.pitchChild;
        }
        else if (out.yawChild != 0)
        {
            // Ravager: the barrel pitches as a child of the yawing turret.
            out.pitchChild =
                makeChild("TF_VehTurretBarrel", static_cast<EntityID>(out.yawChild), spec->pitchPivot, spec->pitchMesh);
        }
    }

    void TFVehicleSystem::ApplyTurretPose(const TurretRig& rig, float hullYaw, float aimYaw, float aimPitch)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return;
        auto& registry = world->GetRegistry();
        const float localYawDeg = QuantAim::WrapPi(aimYaw - hullYaw) * kRadToDeg;
        const float pitchDeg = aimPitch * kRadToDeg; // camera convention == Transform +X

        if (rig.yawChild != 0)
        {
            const auto e = static_cast<EntityID>(rig.yawChild);
            if (registry.valid(e))
                if (Transform* t = world->GetComponent<Transform>(e))
                    t->rotation.y = localYawDeg;
        }
        if (rig.pitchChild != 0)
        {
            const auto e = static_cast<EntityID>(rig.pitchChild);
            if (registry.valid(e))
                if (Transform* t = world->GetComponent<Transform>(e))
                    t->rotation.x = pitchDeg;
        }
    }

    void TFVehicleSystem::DestroyTurretRig(TurretRig& rig)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        // Grandchild barrel first, then turret/head, then the pedestal.
        for (uint32_t* id : {&rig.pitchChild, &rig.yawChild, &rig.baseChild})
        {
            if (*id != 0 && world)
            {
                const auto e = static_cast<EntityID>(*id);
                if (world->GetRegistry().valid(e))
                    world->DestroyEntity(e);
            }
            *id = 0;
        }
    }

    bool TFVehicleSystem::ServerPurchaseVehicle(PlayerId player, VehicleId vehId)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return false;

        const VehicleDef* def = DefOf(vehId);
        if (!def || !def->enabled)
        {
            ++m_purchasesRejected;
            return false;
        }
        if (m_vehicles.size() >= kMaxVehicles)
        {
            ++m_purchasesRejected;
            return false;
        }

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive || pawn.faction == FactionId::None ||
            IsSeated(player))
        {
            ++m_purchasesRejected;
            return false;
        }

        float pad[2];
        if (!FindTerminal(pawn.pos, pawn.faction, kTFVehTerminalRangeM, pad))
        {
            ++m_purchasesRejected;
            return false; // no friendly terminal in reach (also covers region ownership)
        }

        // W6 progression: one-time access gate (per-spawn fluxCost below still applies).
        if (m_ctx->progression && !m_ctx->progression->IsVehicleUnlocked(player, vehId))
        {
            ++m_purchasesRejected;
            return false;
        }

        // Flux gate (ctx.progression is authoritative; absent only in unit tests).
        if (m_ctx->progression && def->fluxCost > 0 &&
            !m_ctx->progression->ServerSpendFlux(player, static_cast<uint32_t>(def->fluxCost)))
        {
            ++m_purchasesRejected;
            return false;
        }

        // Terminal pad with a small search so back-to-back buys don't interpenetrate.
        float pos[3] = {pad[0], 0.0f, pad[1]};
        const float clearance = VehicleRadius(vehId) + 1.0f;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            bool blocked = false;
            for (const VehicleRec& other : m_vehicles)
            {
                const float need = clearance + VehicleRadius(other.vehId);
                if (Dist2XZ(other.pos, pos[0], pos[2]) < need * need)
                {
                    blocked = true;
                    break;
                }
            }
            if (!blocked)
                break;
            pos[0] = std::clamp(pos[0] + clearance * 2.0f, kWorldMin, kWorldMax);
        }
        pos[1] = TerrainAt(pos[0], pos[2]);
        const float yaw = std::atan2(kMapCenter - pos[0], kMapCenter - pos[2]);

        VehicleRec v;
        v.local = CreateVehicleEntity(*def, pawn.faction, pos, yaw, v.rig);
        // Headless unit tests have no ECS world; keep a synthetic non-zero id so
        // the record still round-trips (mirrors TFPlayerSystem's convention).
        static EntityId s_syntheticVehEntity = 2000000;
        v.entity = (v.local != 0) ? v.local : s_syntheticVehEntity++;
        v.vehId = vehId;
        v.faction = pawn.faction;
        v.pos[0] = pos[0];
        v.pos[1] = pos[1];
        v.pos[2] = pos[2];
        v.yaw = yaw;
        v.aimYaw = yaw; // turret starts hull-forward, level
        v.hp = v.maxHp = def->health;
        v.seatCount = static_cast<uint8_t>(std::min<size_t>(def->seats.size(), 8));
        m_vehicles.push_back(v);
        ++m_purchases;

        // Jolt hull for the rigid-body driving path. On failure (or without Jolt)
        // this vehicle simply keeps the math path — nothing else changes.
        if (m_joltDrive)
            m_joltDrive->AttachVehicle(v.entity, vehId, v.pos, yaw, v.local);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] vehicle %u (%s) purchased by player %u at (%.0f %.0f %.0f)",
                       v.entity, def->name.c_str(), player, pos[0], pos[1], pos[2]);

        if (m_events)
            m_events->Fire(EvVehicleSpawned{v.entity, vehId, player});

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
        {
            ServerSendCreate(kInvalidPlayer, m_vehicles.back());
            ServerSendSeats(kInvalidPlayer, m_vehicles.back());
        }
#endif
        return true;
    }

    // ---------------------------------------------------------------------------
    // Weapons integration
    // ---------------------------------------------------------------------------

    bool TFVehicleSystem::GetSeatWeapon(PlayerId player, WeaponId& out) const
    {
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end() || it->second.exiting)
            return false;
        out = kInvalidWeapon;
        const VehicleRec* v = FindRec(it->second.vehicle);
        const VehicleDef* def = v ? DefOf(v->vehId) : nullptr;
        if (!def || it->second.seatIdx >= def->seats.size())
            return true; // seated, unarmed
        const std::string& key = def->seats[it->second.seatIdx].weaponKey;
        if (key.empty() || !m_ctx->data)
            return true; // seated, unarmed
        if (const WeaponDef* w = m_ctx->data->GetWeaponByKey(key))
            out = w->id;
        return true;
    }

    bool TFVehicleSystem::GetSeatFireFrame(PlayerId player, float outOrigin[3], float outDir[3]) const
    {
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end() || it->second.exiting)
            return false;
        const VehicleRec* v = FindRec(it->second.vehicle);
        const VehicleDef* def = v ? DefOf(v->vehId) : nullptr;
        const TurretRigSpec* spec = v ? RigSpecOf(v->vehId) : nullptr;
        if (!v || !def || !spec)
            return false;
        if (static_cast<int>(it->second.seatIdx) != TurretControllerSeat(def))
            return false;

        // Aim direction from the server aim state (camera pitch is positive-down,
        // BuildViewRay convention: dir.y = -sin(pitch)).
        const float cp = std::cos(v->aimPitch);
        outDir[0] = cp * std::sin(v->aimYaw);
        outDir[1] = -std::sin(v->aimPitch);
        outDir[2] = cp * std::cos(v->aimYaw);

        // Pitch-pivot world position: hull-space mounts rotate with the hull yaw;
        // the Ravager barrel pivot is turret-space, so it rotates with the AIM
        // yaw on top of the hull-space turret pivot. Hull pitch/roll are ignored
        // here (same approximation as RaycastVehicles' upright hull spheres).
        float pivot[3] = {v->pos[0], v->pos[1], v->pos[2]};
        float off[3];
        if (spec->headYawsToo)
        {
            YawRotate(spec->pitchPivot, v->yaw, off);
            pivot[0] += off[0];
            pivot[1] += off[1];
            pivot[2] += off[2];
        }
        else
        {
            YawRotate(def->turretPivot, v->yaw, off);
            pivot[0] += off[0];
            pivot[1] += off[1];
            pivot[2] += off[2];
            YawRotate(spec->pitchPivot, v->aimYaw, off);
            pivot[0] += off[0];
            pivot[1] += off[1];
            pivot[2] += off[2];
        }
        outOrigin[0] = pivot[0] + outDir[0] * spec->muzzleM;
        outOrigin[1] = pivot[1] + outDir[1] * spec->muzzleM;
        outOrigin[2] = pivot[2] + outDir[2] * spec->muzzleM;
        return true;
    }

    EntityId TFVehicleSystem::RaycastVehicles(const float origin[3], const float dir[3], float maxDist,
                                              float outHitPoint[3], float* outDist) const
    {
        EntityId best = 0;
        float bestT = maxDist;
        for (const VehicleRec& v : m_vehicles)
        {
            if (v.hp <= 0.0f)
                continue;
            const float r = VehicleRadius(v.vehId);
            const float c[3] = {v.pos[0], v.pos[1] + r * 0.6f, v.pos[2]}; // hull center
            const float oc[3] = {origin[0] - c[0], origin[1] - c[1], origin[2] - c[2]};
            const float b = oc[0] * dir[0] + oc[1] * dir[1] + oc[2] * dir[2];
            const float cc = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - r * r;
            if (cc <= 0.0f)
                continue; // origin inside this hull -> own-vehicle shot, skip
            const float disc = b * b - cc;
            if (disc < 0.0f)
                continue;
            const float t = -b - std::sqrt(disc);
            if (t < 0.0f || t >= bestT)
                continue;
            bestT = t;
            best = v.entity;
        }
        if (best != 0)
        {
            if (outHitPoint)
            {
                outHitPoint[0] = origin[0] + dir[0] * bestT;
                outHitPoint[1] = origin[1] + dir[1] * bestT;
                outHitPoint[2] = origin[2] + dir[2] * bestT;
            }
            if (outDist)
                *outDist = bestT;
        }
        return best;
    }

} // namespace Terrafront
