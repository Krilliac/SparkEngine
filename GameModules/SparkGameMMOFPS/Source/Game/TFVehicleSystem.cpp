/**
 * @file TFVehicleSystem.cpp
 * @brief W3 vehicles — lifecycle + authoritative server core: terminal
 *        purchase, seats/ride sync, driving model, Aegis deploy, damage and
 *        destruction. Replication halves live in TFVehicleNet.cpp, client UX
 *        in TFVehicleClient.cpp, the Jolt rigid-body driving path in
 *        TFVehiclePhysics.cpp (same lane, split per repo file-size rules).
 *        StepVehicleJolt drives when a live Jolt world exists; StepVehicle is
 *        the math fallback so the module works identically without Jolt.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVehiclePhysics.h"
#include "Game/TFVisualUtils.h"
#include "Net/TFServerSim.h"
#include "UI/TFVehicleHUD.h" // complete type for the m_vehicleHud unique_ptr (dtor/Shutdown)
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#include "Audio/AudioEngine.h"
#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;
        constexpr float kWorldMin = 0.0f;
        constexpr float kWorldMax = 4096.0f;
        constexpr float kMapCenter = 2048.0f;
        constexpr float kDriveDrag = 1.5f;      // 1/s throttle-off speed decay
        constexpr float kReverseFactor = 0.4f;  // reverse cap = topSpeed * this
        constexpr float kSteerRefSpeed = 2.0f;  // full steering authority above this
        constexpr float kTiltSampleM = 2.5f;    // terrain slope sample arm
        constexpr float kInputStaleSec = 0.25f; // driver input starvation -> coast
        constexpr float kRideUpM = 0.9f;        // seated pawn feet above vehicle base
        constexpr float kExitSideM = 3.0f;      // exit placement distance from center
        constexpr float kSpawnLiftM = 0.10f;
        constexpr int kMaxVehicles = 64;
        constexpr uint8_t kDamageKindExplosive = 1; // TFNetProtocol damageKind convention

        // VTOL (Vulture) math-path mirrors of the TFVehiclePhysics.cpp Jolt feel
        // constants (same parity duplication convention as kDriveDrag above).
        constexpr float kVtolClimbRate = 8.0f;    // m/s climb at full lift
        constexpr float kVtolDescendRate = 6.0f;  // m/s descent at full negative lift
        constexpr float kVtolAutoDescend = 3.0f;  // m/s driverless / input-starved auto-land
        constexpr float kVtolCeilAGL = 120.0f;    // m max altitude above the terrain under the hull
        constexpr float kVtolWorldCeilY = 200.0f; // m hard absolute world ceiling
        constexpr float kVtolLeanPitch = 0.35f;   // rad nose-down visual lean at full throttle
        constexpr float kVtolLeanRoll = 0.45f;    // rad banking visual lean at full steer
        constexpr float kVtolLeanRate = 5.0f;     // 1/s lean approach speed (math path visual)
        constexpr float kVtolLandedAGL = 2.0f;    // m hull base above ground = landed (exit gate)

        // W13 damage-state performance degradation (server-authoritative). The
        // threshold is INTENTIONALLY the same numeric literal as
        // TFVehicleFx.cpp's kCriticalHpFrac (0.66f/0.33f tiers) so the client-
        // side smoke/fire tier and this movement penalty change state at the
        // same hp fraction (same cross-file duplication convention as the VTOL
        // math-path/Jolt-path constants above -- see DamageMovementMults). Only
        // the critical tier (<=33% hp) carries a movement penalty; the damaged
        // tier (33-66%, TFVehicleFx-only) is visuals with no mechanical effect.
        constexpr float kTFVehCriticalHpFrac = 0.33f;    // <=33% hp -> critical tier (speed/turn penalty)
        constexpr float kTFVehCriticalSpeedMult = 0.70f; // critical: ~30% top-speed loss
        constexpr float kTFVehCriticalTurnMult = 0.75f;  // critical: slower turn authority

        // W13 persistent wreck (kills leave a mark).
        constexpr float kTFVehWreckLifeSec = 15.0f;
        constexpr char kTFVehWreckMaterial[] = "Assets/Materials/MMOFPS/Structure_AlloyDark.json"; // charred tint

        float Dist2XZ(const float a[3], const float bx, const float bz)
        {
            const float dx = a[0] - bx;
            const float dz = a[2] - bz;
            return dx * dx + dz * dz;
        }

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

    TFVehicleSystem::TFVehicleSystem() = default;
    TFVehicleSystem::~TFVehicleSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFVehicleSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Seats never outlive their pawn: death (incl. disconnect cleanup, which
        // kills the pawn) frees the seat without an exit placement.
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

        // Jolt-backed driving model. When there is no live Jolt world this stays
        // null and the original math path (StepVehicle) drives every vehicle.
        auto joltDrive = std::make_unique<TFVehiclePhysics>();
        if (joltDrive->Initialize(ctx))
            m_joltDrive = std::move(joltDrive);

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFVehicleSystem initialized (W3)");
        return true;
    }

    void TFVehicleSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;

        if (!m_ctx->IsAuthority())
            ClientInterpolate(deltaTime);

        if (m_ctx->HasLocalPlayer())
            ClientUpdateUX(deltaTime);

        // W13: persistent wrecks exist on both roles (server hulls + client
        // mirror hulls), so the despawn timer runs unconditionally here.
        UpdateWrecks();
    }

    void TFVehicleSystem::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        NetFixedUpdate(fixedDeltaTime);

        if (!m_ctx->IsAuthority())
            return;

        // Runs AFTER TFServerSim::FixedUpdate (Main.cpp order): seated inputs for
        // this tick are already cached on the vehicles, ride poses were synced,
        // and the world physics step (issued by TFServerSim) has run — so the
        // Jolt path reads back a fresh pose and queues forces for the next step.
        for (VehicleRec& v : m_vehicles)
        {
            const VehicleDef* def = DefOf(v.vehId);
            if (!StepVehicleJolt(v, def))
                StepVehicle(v, def, fixedDeltaTime);
            WriteVehicleTransform(v);
        }

        // Safety sweep: exit latches that TFServerSim never consumed (e.g. the
        // pawn left m_move in the same tick) must not pin IsSeated forever.
        for (auto it = m_seatOf.begin(); it != m_seatOf.end();)
        {
            if (it->second.exiting && ++it->second.exitTicks > 3)
                it = m_seatOf.erase(it);
            else
                ++it;
        }
    }

    void TFVehicleSystem::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        if (m_serverHandlers)
            ServerReleaseNetHandlers();
        if (m_clientHandlers)
            ClientReleaseHandlers();
#endif
        // Destroy server vehicle entities (module teardown; no broadcast needed).
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        for (VehicleRec& v : m_vehicles)
        {
            DestroyTurretRig(v.rig); // rig children (incl. grandchild barrel) first
            const auto e = static_cast<EntityID>(v.local);
            if (world && v.local != 0 && world->GetRegistry().valid(e))
                world->DestroyEntity(e);
        }
        // W13: leftover wreck entities (module teardown; no despawn timer wait).
        for (WreckRec& w : m_wrecks)
        {
            const auto e = static_cast<EntityID>(w.local);
            if (world && w.local != 0 && world->GetRegistry().valid(e))
                world->DestroyEntity(e);
        }
        m_wrecks.clear();
        ClientDropMirror();
        if (m_joltDrive)
        {
            m_joltDrive->Shutdown();
            m_joltDrive.reset();
        }
        m_vehicles.clear();
        m_seatOf.clear();
        m_lastSent.clear();
        m_lastAimSent.clear();
        m_knownClients.clear();
        m_loadedSounds.clear();
        m_vehicleHud.reset();
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------------

    TFVehicleSystem::VehicleRec* TFVehicleSystem::FindRec(EntityId vehicle)
    {
        for (VehicleRec& v : m_vehicles)
            if (v.entity == vehicle)
                return &v;
        return nullptr;
    }

    const TFVehicleSystem::VehicleRec* TFVehicleSystem::FindRec(EntityId vehicle) const
    {
        for (const VehicleRec& v : m_vehicles)
            if (v.entity == vehicle)
                return &v;
        return nullptr;
    }

    const VehicleDef* TFVehicleSystem::DefOf(VehicleId id) const
    {
        return (m_ctx && m_ctx->data && m_ctx->data->IsLoaded()) ? m_ctx->data->GetVehicle(id) : nullptr;
    }

    float TFVehicleSystem::TerrainAt(float x, float z) const
    {
        return (m_ctx && m_ctx->world) ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f;
    }

    float TFVehicleSystem::VehicleRadius(VehicleId id) const
    {
        // Bounding-sphere radii for hit tests / enter reach (hull approximations;
        // OBJ footprints, not balance data).
        switch (id)
        {
        case VehicleId::Drifter:
            return 1.6f;
        case VehicleId::Aegis:
            return 3.2f;
        case VehicleId::Ravager:
            return 2.6f;
        case VehicleId::Vulture:
            return 2.8f;
        default:
            return 2.0f;
        }
    }

    void TFVehicleSystem::RidePos(const VehicleRec& v, float out[3]) const
    {
        out[0] = v.pos[0];
        out[1] = v.pos[1] + kRideUpM;
        out[2] = v.pos[2];
    }

    void TFVehicleSystem::ExitPosFor(const VehicleRec& v, uint8_t seatIdx, float out[3]) const
    {
        // Alternate sides per seat so a full Aegis does not stack 8 pawns.
        const float side = (seatIdx % 2 == 0) ? 1.0f : -1.0f;
        const float rightX = std::cos(v.yaw) * side;
        const float rightZ = -std::sin(v.yaw) * side;
        const float dist = VehicleRadius(v.vehId) + kExitSideM - 1.5f;
        out[0] = std::clamp(v.pos[0] + rightX * dist, kWorldMin, kWorldMax);
        out[2] = std::clamp(v.pos[2] + rightZ * dist, kWorldMin, kWorldMax);
        out[1] = TerrainAt(out[0], out[2]) + kSpawnLiftM;
    }

    bool TFVehicleSystem::FindTerminal(const float pawnPos[3], FactionId faction, float maxRangeM,
                                       float outPad[2]) const
    {
        if (!m_ctx->data || !m_ctx->data->IsLoaded() || !m_ctx->regions)
            return false;
        const float maxR2 = maxRangeM * maxRangeM;
        bool found = false;
        float best = maxR2;
        for (const RegionDef& r : m_ctx->data->GetContinent().regions)
        {
            if (!r.vehicleTerminal.has_value())
                continue;
            if (m_ctx->regions->OwnerOf(r.id) != faction)
                continue;
            const float tx = (*r.vehicleTerminal)[0];
            const float tz = (*r.vehicleTerminal)[1];
            const float d2 = Dist2XZ(pawnPos, tx, tz);
            if (d2 <= best)
            {
                best = d2;
                outPad[0] = tx;
                outPad[1] = tz;
                found = true;
            }
        }
        return found;
    }

    void TFVehicleSystem::PlayOneShot(const std::string& assetsRelPath)
    {
        if (assetsRelPath.empty() || !m_ctx || !m_ctx->engine || !m_ctx->HasLocalPlayer())
            return;
        ::AudioEngine* audio = m_ctx->engine->GetAudio();
        if (!audio)
            return;
        if (m_loadedSounds.insert(assetsRelPath).second)
        {
            const std::string full = "Assets/" + assetsRelPath; // vehicles.json paths are Assets-relative
            audio->LoadSound(assetsRelPath, std::wstring(full.begin(), full.end()));
        }
        audio->PlaySound(assetsRelPath, 0.9f);
    }

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
    // Seats
    // ---------------------------------------------------------------------------

    bool TFVehicleSystem::IsSeated(PlayerId player) const
    {
        if (m_seatOf.contains(player))
            return true;
        // Pure client: derive from the replicated seat tables.
        for (const auto& [entity, seats] : m_mirrorSeats)
            for (uint8_t i = 0; i < seats.seatCount && i < 8; ++i)
                if (seats.seats[i] == player)
                    return true;
        return false;
    }

    void TFVehicleSystem::ServerHandleSeatOp(PlayerId player, const TF_VehicleSeatOp& op, bool enter)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return;
        ++m_seatOps;

        if (!enter)
        {
            // Exit ignores op.vehicleEntity: you can only leave the seat you hold.
            auto it = m_seatOf.find(player);
            if (it == m_seatOf.end() || it->second.exiting)
                return;
            // VTOL: no bailing out mid-flight — the Vulture must be landed
            // (destruction still ejects everyone through UnseatPlayer directly).
            if (const VehicleRec* v = FindRec(it->second.vehicle);
                v && v->vehId == VehicleId::Vulture && !VehicleLanded(*v))
                return;
            UnseatPlayer(player, true);
            return;
        }

        // W10 seat swap: VehicleEnter while ALREADY seated in the named vehicle
        // moves the player to op.seatIndex without exiting (previously a silent
        // no-op, so the message reuse is backward-compatible). Validation is
        // strict — same vehicle, seat exists, not the current seat, and empty;
        // a failed swap keeps the current seat (no first-free fallback).
        if (auto sit = m_seatOf.find(player); sit != m_seatOf.end())
        {
            SeatRef& ref = sit->second;
            if (ref.exiting || op.vehicleEntity != ref.vehicle)
                return;
            VehicleRec* sv = FindRec(ref.vehicle);
            if (!sv || sv->hp <= 0.0f)
                return;
            if (op.seatIndex >= sv->seatCount || op.seatIndex == ref.seatIdx ||
                sv->seats[op.seatIndex] != kInvalidPlayer)
                return;
            if (ref.seatIdx < 8 && sv->seats[ref.seatIdx] == player)
                sv->seats[ref.seatIdx] = kInvalidPlayer;
            if (ref.seatIdx == 0)
            {
                // vacating the driver seat kills the cached drive inputs (same
                // as UnseatPlayer) — a Vulture starts its driverless auto-land.
                sv->throttle = 0.0f;
                sv->steer = 0.0f;
                sv->lift = 0.0f;
            }
            sv->seats[op.seatIndex] = player;
            sv->seatsDirty = true;
            ref.seatIdx = op.seatIndex;
            WriteSeatComp(player, sv->entity, op.seatIndex);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u swapped to seat %u of vehicle %u", player,
                           static_cast<unsigned>(op.seatIndex), sv->entity);
            return;
        }

        VehicleRec* v = FindRec(op.vehicleEntity);
        if (!v || v->hp <= 0.0f)
            return;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
            return;
        if (pawn.faction != v->faction)
            return; // no cross-faction theft in W3

        const float reach = VehicleRadius(v->vehId) + kTFVehEnterRangeM;
        if (Dist2XZ(pawn.pos, v->pos[0], v->pos[2]) > reach * reach)
            return;
        // VTOL: the XZ reach test alone would let a pawn board a Vulture hovering
        // 100 m straight above — flying hulls also need vertical proximity.
        if (v->vehId == VehicleId::Vulture && std::fabs(pawn.pos[1] - v->pos[1]) > reach)
            return;

        // Requested seat if free, else first free (driver seat 0 first).
        int seat = -1;
        if (op.seatIndex < v->seatCount && v->seats[op.seatIndex] == kInvalidPlayer)
            seat = op.seatIndex;
        else
            for (uint8_t i = 0; i < v->seatCount; ++i)
                if (v->seats[i] == kInvalidPlayer)
                {
                    seat = i;
                    break;
                }
        if (seat < 0)
            return; // full

        v->seats[seat] = player;
        v->seatsDirty = true;
        SeatRef ref;
        ref.vehicle = v->entity;
        ref.seatIdx = static_cast<uint8_t>(seat);
        m_seatOf[player] = ref;

        WriteSeatComp(player, v->entity, static_cast<uint8_t>(seat));

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u entered vehicle %u seat %d", player, v->entity, seat);
    }

    void TFVehicleSystem::WriteSeatComp(PlayerId player, EntityId vehicle, uint8_t seatIdx)
    {
        // ECS mirror on the pawn (TFComponents contract) — shared by the enter
        // and W10 swap paths. No-op headless (no world) or with no live pawn.
        if (!m_ctx || !m_ctx->players)
            return;
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        PawnInfo pawn{};
        uint32_t local = 0;
        if (!world || !m_ctx->players->GetPawnByPlayer(player, pawn) ||
            !m_ctx->players->ResolveEntity(pawn.entity, local))
            return;
        const auto e = static_cast<EntityID>(local);
        if (!world->GetRegistry().valid(e))
            return;
        TFSeatComp& sc = world->HasComponent<TFSeatComp>(e) ? *world->GetComponent<TFSeatComp>(e)
                                                            : world->AddComponent<TFSeatComp>(e);
        sc.vehicle = vehicle;
        sc.seatIdx = seatIdx;
    }

    void TFVehicleSystem::UnseatPlayer(PlayerId player, bool placeBeside)
    {
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end())
            return;
        SeatRef& ref = it->second;
        VehicleRec* v = FindRec(ref.vehicle);
        if (v && ref.seatIdx < 8 && v->seats[ref.seatIdx] == player)
        {
            v->seats[ref.seatIdx] = kInvalidPlayer;
            v->seatsDirty = true;
            if (ref.seatIdx == 0)
            {
                v->throttle = 0.0f;
                v->steer = 0.0f;
                v->lift = 0.0f;
            }
        }

        // Drop the pawn's ECS seat marker.
        if (m_ctx && m_ctx->players && m_ctx->engine)
        {
            PawnInfo pawn{};
            uint32_t local = 0;
            if (m_ctx->players->GetPawnByPlayer(player, pawn) && m_ctx->players->ResolveEntity(pawn.entity, local))
            {
                World* world = m_ctx->engine->GetWorld();
                const auto e = static_cast<EntityID>(local);
                if (world && world->GetRegistry().valid(e) && world->HasComponent<TFSeatComp>(e))
                    world->RemoveComponent<TFSeatComp>(e);
            }
        }

        if (placeBeside && v)
        {
            // One-tick exit latch: IsSeated stays true until TFServerSim pulls the
            // exit placement through SyncSeatedPawn (see header contract).
            ref.exiting = true;
            ref.exitTicks = 0;
            ExitPosFor(*v, ref.seatIdx, ref.exitPos);
        }
        else
        {
            m_seatOf.erase(it);
        }
    }

    void TFVehicleSystem::ServerHandleSeatedInput(PlayerId player, const TF_ClientInput& input, float dt)
    {
        (void)dt; // integration happens once per fixed tick in StepVehicle
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end() || it->second.exiting)
            return;

        VehicleRec* v = FindRec(it->second.vehicle);
        if (!v)
            return;

        // W8 turret aim: the controller seat's view angles drive the turret.
        // Guard non-finite angles OURSELVES — TFServerSim's seated-path guard
        // runs after this call, so a poisoned viewYaw would reach WrapPi's
        // non-terminating loop here otherwise.
        if (static_cast<int>(it->second.seatIdx) == TurretControllerSeat(DefOf(v->vehId)) &&
            std::isfinite(input.viewYaw) && std::isfinite(input.viewPitch))
        {
            v->aimYaw = QuantAim::WrapPi(input.viewYaw);
            v->aimPitch = std::clamp(input.viewPitch, kTFTurretPitchMinRad, kTFTurretPitchMaxRad);
        }

        if (it->second.seatIdx != 0)
            return; // non-driver seats: aim captured above, fire via the weapon path

        v->throttle = std::clamp(static_cast<float>(input.moveY) / 127.0f, -1.0f, 1.0f);
        v->steer = std::clamp(static_cast<float>(input.moveX) / 127.0f, -1.0f, 1.0f);
        // Vertical axis for VTOL hulls: Jump climbs, Crouch descends (the seated
        // pawn is not walking, so these bits are otherwise unused while driving).
        v->lift = ((input.buttons & TFB_Jump) != 0 ? 1.0f : 0.0f) - ((input.buttons & TFB_Crouch) != 0 ? 1.0f : 0.0f);
        v->lastDriveInput = (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
    }

    void TFVehicleSystem::SyncSeatedPawn(PlayerId player, float outPos[3], float outVel[3])
    {
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end())
            return; // leave the caller's state untouched

        if (it->second.exiting)
        {
            outPos[0] = it->second.exitPos[0];
            outPos[1] = it->second.exitPos[1];
            outPos[2] = it->second.exitPos[2];
            outVel[0] = outVel[1] = outVel[2] = 0.0f;
            m_seatOf.erase(it); // latch consumed — walking resumes next tick
            return;
        }

        const VehicleRec* v = FindRec(it->second.vehicle);
        if (!v)
        {
            m_seatOf.erase(it); // vehicle vanished without an eject (defensive)
            return;
        }
        RidePos(*v, outPos);
        outVel[0] = std::sin(v->yaw) * v->speed;
        outVel[1] = 0.0f;
        outVel[2] = std::cos(v->yaw) * v->speed;
    }

    // ---------------------------------------------------------------------------
    // Driving
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::DamageMovementMults(const VehicleRec& v, float& outSpeedMult, float& outTurnMult) const
    {
        outSpeedMult = 1.0f;
        outTurnMult = 1.0f;
        if (v.maxHp <= 0.0f)
            return;
        if (v.hp / v.maxHp <= kTFVehCriticalHpFrac)
        {
            outSpeedMult = kTFVehCriticalSpeedMult;
            outTurnMult = kTFVehCriticalTurnMult;
        }
    }

    void TFVehicleSystem::StepVehicle(VehicleRec& v, const VehicleDef* def, float dt)
    {
        if (v.hp <= 0.0f)
            return;

        float speedMult = 1.0f, turnMult = 1.0f;
        DamageMovementMults(v, speedMult, turnMult);
        const float topSpeed = (def ? def->topSpeed : 10.0f) * speedMult;
        const float accel = def ? def->accel : 5.0f;
        const float turnRate = (def ? def->turnRate : 1.5f) * turnMult;

        const double now = (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
        const bool driven = v.seats[0] != kInvalidPlayer && !v.deployed && (now - v.lastDriveInput) < kInputStaleSec;
        const bool vtol = v.vehId == VehicleId::Vulture;

        if (driven)
        {
            v.speed += v.throttle * accel * dt;
            if (vtol)
            {
                // A gunship yaws in place: full authority at any speed.
                v.yaw = QuantAim::WrapPi(v.yaw + v.steer * turnRate * dt);
            }
            else
            {
                // Steering authority scales in with speed; reversing mirrors the wheel.
                const float authority = std::clamp(std::fabs(v.speed) / kSteerRefSpeed, 0.0f, 1.0f);
                const float dir = (v.speed >= 0.0f) ? 1.0f : -1.0f;
                v.yaw = QuantAim::WrapPi(v.yaw + v.steer * turnRate * authority * dir * dt);
            }
        }
        else
        {
            const float keep = std::max(0.0f, 1.0f - kDriveDrag * dt);
            v.speed *= keep;
            if (std::fabs(v.speed) < 0.05f)
                v.speed = 0.0f;
        }
        v.speed = std::clamp(v.speed, -topSpeed * kReverseFactor, topSpeed);

        const float fwdX = std::sin(v.yaw);
        const float fwdZ = std::cos(v.yaw);
        v.pos[0] = std::clamp(v.pos[0] + fwdX * v.speed * dt, kWorldMin, kWorldMax);
        v.pos[2] = std::clamp(v.pos[2] + fwdZ * v.speed * dt, kWorldMin, kWorldMax);

        if (vtol)
        {
            // VTOL altitude: Jump/Crouch climb or descend, hold at lift 0,
            // auto-land when driverless; AGL ceiling + hard world ceiling.
            const float terrain = TerrainAt(v.pos[0], v.pos[2]);
            const float vy = driven ? (v.lift > 0.0f   ? v.lift * kVtolClimbRate
                                       : v.lift < 0.0f ? v.lift * kVtolDescendRate
                                                       : 0.0f)
                                    : -kVtolAutoDescend;
            const float ceilY = std::min(terrain + kVtolCeilAGL, kVtolWorldCeilY);
            v.pos[1] = std::clamp(v.pos[1] + vy * dt, terrain, std::max(terrain, ceilY));

            // Visual lean: forward tilt with throttle, banking with steer
            // (signs mirror the Jolt lean servo; positive pitch = nose down,
            // positive roll = lean left in this module's readback convention).
            const float k = std::min(1.0f, kVtolLeanRate * dt);
            const float targetPitch = driven ? v.throttle * kVtolLeanPitch : 0.0f;
            const float targetRoll = driven ? -v.steer * kVtolLeanRoll : 0.0f;
            v.pitch += (targetPitch - v.pitch) * k;
            v.roll += (targetRoll - v.roll) * k;
            return;
        }

        v.pos[1] = TerrainAt(v.pos[0], v.pos[2]);

        // Visual pitch/roll from the terrain slope under the hull.
        const float hF = TerrainAt(v.pos[0] + fwdX * kTiltSampleM, v.pos[2] + fwdZ * kTiltSampleM);
        const float hB = TerrainAt(v.pos[0] - fwdX * kTiltSampleM, v.pos[2] - fwdZ * kTiltSampleM);
        const float rX = fwdZ, rZ = -fwdX; // right vector
        const float hR = TerrainAt(v.pos[0] + rX * kTiltSampleM, v.pos[2] + rZ * kTiltSampleM);
        const float hL = TerrainAt(v.pos[0] - rX * kTiltSampleM, v.pos[2] - rZ * kTiltSampleM);
        v.pitch = std::atan2(hB - hF, 2.0f * kTiltSampleM); // nose up on uphill
        v.roll = std::atan2(hR - hL, 2.0f * kTiltSampleM);
    }

    bool TFVehicleSystem::StepVehicleJolt(VehicleRec& v, const VehicleDef* def)
    {
        if (!m_joltDrive)
            return false;
        if (v.hp <= 0.0f)
            return true; // destroyed hulls are erased synchronously; never simulate one

        // Same "driver + fresh input + not deployed" gate as the math path.
        const double now = (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;

        TFVehicleDriveState s;
        s.throttle = v.throttle;
        s.steer = v.steer;
        s.lift = v.lift;
        s.deployed = v.deployed;
        s.driven = v.seats[0] != kInvalidPlayer && !v.deployed && (now - v.lastDriveInput) < kInputStaleSec;
        if (def)
        {
            // Same critical-tier speed/turn penalty as the math path (see
            // DamageMovementMults) -- both driving paths run only on the
            // authority role, so applying the identical multiplier here keeps
            // them bit-for-bit consistent regardless of which one executes.
            float speedMult = 1.0f, turnMult = 1.0f;
            DamageMovementMults(v, speedMult, turnMult);
            s.topSpeed = def->topSpeed * speedMult;
            s.accel = def->accel;
            s.turnRate = def->turnRate * turnMult;
        }
        s.pos[0] = v.pos[0];
        s.pos[1] = v.pos[1];
        s.pos[2] = v.pos[2];
        s.yaw = v.yaw;
        s.pitch = v.pitch;
        s.roll = v.roll;
        s.speed = v.speed;

        if (!m_joltDrive->TickVehicle(v.entity, s))
            return false; // no hull body (attach failed) — math fallback

        v.pos[0] = s.pos[0];
        v.pos[1] = s.pos[1];
        v.pos[2] = s.pos[2];
        v.yaw = s.yaw;
        v.pitch = s.pitch;
        v.roll = s.roll;
        v.speed = s.speed;
        return true;
    }

    bool TFVehicleSystem::VehicleLanded(const VehicleRec& v) const
    {
        // Jolt hull: physics-aware clearance (a Vulture parked on a pad or roof
        // counts as landed). Math path: analytic terrain under the hull base.
        float clearance = 0.0f;
        if (m_joltDrive && m_joltDrive->GroundClearanceOf(v.entity, clearance))
            return clearance <= kVtolLandedAGL;
        return v.pos[1] - TerrainAt(v.pos[0], v.pos[2]) <= kVtolLandedAGL;
    }

    void TFVehicleSystem::WriteVehicleTransform(VehicleRec& v)
    {
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || v.local == 0)
            return;
        const auto e = static_cast<EntityID>(v.local);
        if (!world->GetRegistry().valid(e))
            return;
        if (Transform* t = world->GetComponent<Transform>(e))
        {
            t->position = {v.pos[0], v.pos[1], v.pos[2]};
            t->rotation = {v.pitch * kRadToDeg, v.yaw * kRadToDeg, v.roll * kRadToDeg};
        }
        if (HealthComponent* hc = world->GetComponent<HealthComponent>(e))
        {
            hc->health = v.hp;
            hc->isDead = v.hp <= 0.0f;
        }
        if (TFVehicleComp* vc = world->GetComponent<TFVehicleComp>(e))
            std::memcpy(vc->seats, v.seats, sizeof(vc->seats));
        if (TFAegisDeployComp* dc = world->GetComponent<TFAegisDeployComp>(e))
        {
            const bool was = dc->active;
            dc->active = v.deployed;
            // Toggle the deployed-state pylon child on state change. The child is
            // hull-parented, so DestroyVehicle's child-sweep also cleans it up.
            TFVehicleComp* vc = world->GetComponent<TFVehicleComp>(e);
            const VehicleDef* def = vc ? DefOf(vc->vehId) : nullptr;
            if (v.deployed != was && def && !def->deployMesh.empty())
            {
                const std::string pylonPath = "Assets/" + def->deployMesh;
                auto& registry = world->GetRegistry();
                if (v.deployed)
                {
                    FactionId fac = FactionId::None;
                    if (TFFactionComp* fc = world->GetComponent<TFFactionComp>(e))
                        fac = fc->faction;
                    const auto pylon = world->CreateEntity("TF_AegisPylons");
                    Transform& pt = world->AddComponent<Transform>(pylon);
                    pt.parent = e;
                    MeshRenderer& pmr = world->AddComponent<MeshRenderer>(pylon);
                    pmr.meshPath = pylonPath;
                    pmr.materialPath = FactionStructureMaterial(*m_ctx, fac);
                    pmr.castShadows = true;
                }
                else
                {
                    std::vector<EntityID> pylons;
                    for (auto child : world->GetEntitiesWith<Transform, MeshRenderer>())
                    {
                        if (registry.get<Transform>(child).parent == e &&
                            registry.get<MeshRenderer>(child).meshPath == pylonPath)
                            pylons.push_back(child);
                    }
                    for (auto child : pylons)
                        world->DestroyEntity(child);
                }
            }
        }

        // W8: aim the turret rig (server visuals; clients mirror via 0x5448).
        ApplyTurretPose(v.rig, v.yaw, v.aimYaw, v.aimPitch);
    }

    // ---------------------------------------------------------------------------
    // Aegis deploy-spawn
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ServerHandleAegisDeploy(PlayerId player, const TF_AegisDeploy& msg)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        VehicleRec* v = FindRec(msg.vehicleEntity);
        const VehicleDef* def = v ? DefOf(v->vehId) : nullptr;
        if (!v || !def || !def->hasDeploySpawn || v->hp <= 0.0f)
            return;
        if (v->seatCount == 0 || v->seats[0] != player)
            return; // driver only

        const bool wantDeploy = msg.deploy != 0;
        if (wantDeploy == v->deployed)
            return;
        if (wantDeploy && std::fabs(v->speed) > kTFVehDeployMaxSpeed)
            return; // must be stopped

        v->deployed = wantDeploy;
        if (wantDeploy)
            v->speed = 0.0f;
        v->seatsDirty = true; // seats message carries the deploy flag reliably
        if (m_joltDrive)
            m_joltDrive->SetDeployed(v->entity, wantDeploy); // freeze/unfreeze the hull

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Aegis %u %s by player %u", v->entity,
                       wantDeploy ? "DEPLOYED" : "undeployed", player);
        if (m_events)
            m_events->Fire(EvAegisDeployed{v->entity, wantDeploy});
    }

    bool TFVehicleSystem::GetAegisSpawnPos(EntityId vehicle, FactionId faction, float out[3]) const
    {
        const VehicleRec* v = FindRec(vehicle);
        const VehicleDef* def = v ? DefOf(v->vehId) : nullptr;
        if (!v || !def || !def->hasDeploySpawn || !v->deployed || v->hp <= 0.0f)
            return false;
        if (v->faction != faction)
            return false;
        ExitPosFor(*v, 1, out); // spawn on the side pad, terrain height
        return true;
    }

    float TFVehicleSystem::AegisRespawnDelaySec() const
    {
        const VehicleDef* def = DefOf(VehicleId::Aegis);
        return (def && def->deployRespawnSec > 0.0f) ? def->deployRespawnSec : 5.0f;
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

    // ---------------------------------------------------------------------------
    // Events / shared queries
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        UnseatPlayer(ev.victim, false); // corpse doesn't need an exit placement
    }

    void TFVehicleSystem::ForEachVehicle(const std::function<void(const TFVehicleInfo&)>& fn) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            for (const VehicleRec& v : m_vehicles)
            {
                TFVehicleInfo info;
                info.entity = v.entity;
                info.vehId = v.vehId;
                info.faction = v.faction;
                info.pos[0] = v.pos[0];
                info.pos[1] = v.pos[1];
                info.pos[2] = v.pos[2];
                info.yaw = v.yaw;
                info.hp = v.hp;
                info.maxHp = v.maxHp;
                info.deployed = v.deployed;
                std::memcpy(info.seats, v.seats, sizeof(info.seats));
                info.seatCount = v.seatCount;
                fn(info);
            }
            return;
        }
        for (const auto& [entity, m] : m_mirror)
            fn(m.info);
    }

    bool TFVehicleSystem::GetVehicleInfo(EntityId vehicle, TFVehicleInfo& out) const
    {
        bool found = false;
        ForEachVehicle(
            [&](const TFVehicleInfo& info)
            {
                if (!found && info.entity == vehicle)
                {
                    out = info;
                    found = true;
                }
            });
        return found;
    }

    bool TFVehicleSystem::GetSeatedVehicleHp(PlayerId player, float& outCur, float& outMax) const
    {
        auto it = m_seatOf.find(player);
        if (it != m_seatOf.end())
        {
            const VehicleRec* v = FindRec(it->second.vehicle);
            if (!v)
                return false;
            outCur = v->hp;
            outMax = v->maxHp;
            return true;
        }
        // Pure client: replicated seat table + mirror hp.
        for (const auto& [entity, seats] : m_mirrorSeats)
            for (uint8_t i = 0; i < seats.seatCount && i < 8; ++i)
                if (seats.seats[i] == player)
                {
                    auto mit = m_mirror.find(entity);
                    if (mit == m_mirror.end())
                        return false;
                    outCur = mit->second.info.hp;
                    outMax = mit->second.info.maxHp;
                    return true;
                }
        return false;
    }

} // namespace Terrafront
