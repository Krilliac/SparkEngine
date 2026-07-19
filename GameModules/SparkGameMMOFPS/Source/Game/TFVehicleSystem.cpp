/**
 * @file TFVehicleSystem.cpp
 * @brief W3 vehicles — lifecycle + shared server core: init/update/shutdown,
 *        record/terminal/audio helpers and the shared vehicle queries. Same
 *        class, split per repo file-size rules: purchase + turret rig in
 *        TFVehicleSystemSpawn.cpp, seats/Aegis in TFVehicleSystemSeats.cpp,
 *        the driving model in TFVehicleSystemDrive.cpp, damage/destruction in
 *        TFVehicleSystemDamage.cpp (shared internals in
 *        TFVehicleSystemInternal.h), replication in TFVehicleNet.cpp, client
 *        UX in TFVehicleClient.cpp and the Jolt rigid-body driving path in
 *        TFVehiclePhysics.cpp. StepVehicleJolt drives when a live Jolt world
 *        exists; StepVehicle is the math fallback so the module works
 *        identically without Jolt.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFVehiclePhysics.h"
#include "Game/TFVehicleSystemInternal.h"
#include "UI/TFVehicleHUD.h" // complete type for the m_vehicleHud unique_ptr (dtor/Shutdown)
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#include "Audio/AudioEngine.h"
#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Terrafront
{

    using namespace VehicleDetail;

    namespace
    {

        constexpr float kRideUpM = 0.9f;   // seated pawn feet above vehicle base
        constexpr float kExitSideM = 3.0f; // exit placement distance from center
        constexpr float kSpawnLiftM = 0.10f;

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
