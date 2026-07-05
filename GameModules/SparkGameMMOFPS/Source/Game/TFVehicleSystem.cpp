/**
 * @file TFVehicleSystem.cpp
 * @brief W3 vehicles — lifecycle + authoritative server core: terminal
 *        purchase, seats/ride sync, driving model, Aegis deploy, damage and
 *        destruction. Replication halves live in TFVehicleNet.cpp, client UX
 *        in TFVehicleClient.cpp (same class, split per repo file-size rules).
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Net/TFServerSim.h"
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

namespace Terrafront {

namespace {

constexpr float kRadToDeg        = 57.2957795f;
constexpr float kWorldMin        = 0.0f;
constexpr float kWorldMax        = 4096.0f;
constexpr float kMapCenter       = 2048.0f;
constexpr float kDriveDrag       = 1.5f;    // 1/s throttle-off speed decay
constexpr float kReverseFactor   = 0.4f;    // reverse cap = topSpeed * this
constexpr float kSteerRefSpeed   = 2.0f;    // full steering authority above this
constexpr float kTiltSampleM     = 2.5f;    // terrain slope sample arm
constexpr float kInputStaleSec   = 0.25f;   // driver input starvation -> coast
constexpr float kRideUpM         = 0.9f;    // seated pawn feet above vehicle base
constexpr float kExitSideM       = 3.0f;    // exit placement distance from center
constexpr float kSpawnLiftM      = 0.10f;
constexpr int   kMaxVehicles     = 64;
constexpr uint8_t kDamageKindExplosive = 1; // TFNetProtocol damageKind convention

float Dist2XZ(const float a[3], const float bx, const float bz)
{
    const float dx = a[0] - bx;
    const float dz = a[2] - bz;
    return dx * dx + dz * dz;
}

} // namespace

TFVehicleSystem::TFVehicleSystem() = default;
TFVehicleSystem::~TFVehicleSystem() { if (m_initialized) Shutdown(); }

bool TFVehicleSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    // Seats never outlive their pawn: death (incl. disconnect cleanup, which
    // kills the pawn) frees the seat without an exit placement.
    events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

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
}

void TFVehicleSystem::FixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || !m_ctx)
        return;

    NetFixedUpdate(fixedDeltaTime);

    if (!m_ctx->IsAuthority())
        return;

    // Runs AFTER TFServerSim::FixedUpdate (Main.cpp order): seated inputs for
    // this tick are already cached on the vehicles, ride poses were synced.
    for (VehicleRec& v : m_vehicles)
    {
        StepVehicle(v, DefOf(v.vehId), fixedDeltaTime);
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
        const auto e = static_cast<EntityID>(v.local);
        if (world && v.local != 0 && world->GetRegistry().valid(e))
            world->DestroyEntity(e);
    }
    ClientDropMirror();
    m_vehicles.clear();
    m_seatOf.clear();
    m_lastSent.clear();
    m_knownClients.clear();
    m_loadedSounds.clear();
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
    return (m_ctx && m_ctx->data && m_ctx->data->IsLoaded()) ? m_ctx->data->GetVehicle(id)
                                                             : nullptr;
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
        case VehicleId::Drifter: return 1.6f;
        case VehicleId::Aegis:   return 3.2f;
        case VehicleId::Ravager: return 2.6f;
        case VehicleId::Vulture: return 2.8f;
        default:                 return 2.0f;
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

bool TFVehicleSystem::FindTerminal(const float pawnPos[3], FactionId faction,
                                   float maxRangeM, float outPad[2]) const
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

uint32_t TFVehicleSystem::CreateVehicleEntity(const VehicleDef& def, FactionId faction,
                                              const float pos[3], float yaw)
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
        switch (faction)
        {
            case FactionId::MRA: mr.materialPath = "Assets/Materials/MMOFPS/Structure_Metal.json"; break;
            case FactionId::AUC: mr.materialPath = "Assets/Materials/MMOFPS/Structure_Panel.json"; break;
            default:             mr.materialPath = "Assets/Materials/MMOFPS/Structure_Concrete.json"; break;
        }
        mr.castShadows = true;
    }
    return static_cast<uint32_t>(e);
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
    if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive ||
        pawn.faction == FactionId::None || IsSeated(player))
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
    v.local = CreateVehicleEntity(*def, pawn.faction, pos, yaw);
    // Headless unit tests have no ECS world; keep a synthetic non-zero id so
    // the record still round-trips (mirrors TFPlayerSystem's convention).
    static EntityId s_syntheticVehEntity = 2000000;
    v.entity = (v.local != 0) ? v.local : s_syntheticVehEntity++;
    v.vehId = vehId;
    v.faction = pawn.faction;
    v.pos[0] = pos[0]; v.pos[1] = pos[1]; v.pos[2] = pos[2];
    v.yaw = yaw;
    v.hp = v.maxHp = def->health;
    v.seatCount = static_cast<uint8_t>(std::min<size_t>(def->seats.size(), 8));
    m_vehicles.push_back(v);
    ++m_purchases;

    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] vehicle %u (%s) purchased by player %u at (%.0f %.0f %.0f)",
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
        UnseatPlayer(player, true);
        return;
    }

    if (m_seatOf.contains(player))
        return; // already seated
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

    // Requested seat if free, else first free (driver seat 0 first).
    int seat = -1;
    if (op.seatIndex < v->seatCount && v->seats[op.seatIndex] == kInvalidPlayer)
        seat = op.seatIndex;
    else
        for (uint8_t i = 0; i < v->seatCount; ++i)
            if (v->seats[i] == kInvalidPlayer) { seat = i; break; }
    if (seat < 0)
        return; // full

    v->seats[seat] = player;
    v->seatsDirty = true;
    SeatRef ref;
    ref.vehicle = v->entity;
    ref.seatIdx = static_cast<uint8_t>(seat);
    m_seatOf[player] = ref;

    // ECS mirror on the pawn (TFComponents contract).
    uint32_t local = 0;
    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
    if (world && m_ctx->players->ResolveEntity(pawn.entity, local))
    {
        const auto e = static_cast<EntityID>(local);
        if (world->GetRegistry().valid(e))
        {
            TFSeatComp& sc = world->HasComponent<TFSeatComp>(e)
                                 ? *world->GetComponent<TFSeatComp>(e)
                                 : world->AddComponent<TFSeatComp>(e);
            sc.vehicle = v->entity;
            sc.seatIdx = static_cast<uint8_t>(seat);
        }
    }

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u entered vehicle %u seat %d",
                   player, v->entity, seat);
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
        }
    }

    // Drop the pawn's ECS seat marker.
    if (m_ctx && m_ctx->players && m_ctx->engine)
    {
        PawnInfo pawn{};
        uint32_t local = 0;
        if (m_ctx->players->GetPawnByPlayer(player, pawn) &&
            m_ctx->players->ResolveEntity(pawn.entity, local))
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

void TFVehicleSystem::ServerHandleSeatedInput(PlayerId player, const TF_ClientInput& input,
                                              float dt)
{
    (void)dt; // integration happens once per fixed tick in StepVehicle
    auto it = m_seatOf.find(player);
    if (it == m_seatOf.end() || it->second.exiting)
        return;
    if (it->second.seatIdx != 0)
        return; // gunners/passengers aim + fire via the weapon path only

    VehicleRec* v = FindRec(it->second.vehicle);
    if (!v)
        return;
    v->throttle = std::clamp(static_cast<float>(input.moveY) / 127.0f, -1.0f, 1.0f);
    v->steer    = std::clamp(static_cast<float>(input.moveX) / 127.0f, -1.0f, 1.0f);
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

void TFVehicleSystem::StepVehicle(VehicleRec& v, const VehicleDef* def, float dt)
{
    if (v.hp <= 0.0f)
        return;

    const float topSpeed = def ? def->topSpeed : 10.0f;
    const float accel    = def ? def->accel : 5.0f;
    const float turnRate = def ? def->turnRate : 1.5f;

    const double now = (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
    const bool driven = v.seats[0] != kInvalidPlayer && !v.deployed &&
                        (now - v.lastDriveInput) < kInputStaleSec;

    if (driven)
    {
        v.speed += v.throttle * accel * dt;
        // Steering authority scales in with speed; reversing mirrors the wheel.
        const float authority = std::clamp(std::fabs(v.speed) / kSteerRefSpeed, 0.0f, 1.0f);
        const float dir = (v.speed >= 0.0f) ? 1.0f : -1.0f;
        v.yaw = QuantAim::WrapPi(v.yaw + v.steer * turnRate * authority * dir * dt);
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
    v.pos[1] = TerrainAt(v.pos[0], v.pos[2]);

    // Visual pitch/roll from the terrain slope under the hull.
    const float hF = TerrainAt(v.pos[0] + fwdX * kTiltSampleM, v.pos[2] + fwdZ * kTiltSampleM);
    const float hB = TerrainAt(v.pos[0] - fwdX * kTiltSampleM, v.pos[2] - fwdZ * kTiltSampleM);
    const float rX = fwdZ, rZ = -fwdX; // right vector
    const float hR = TerrainAt(v.pos[0] + rX * kTiltSampleM, v.pos[2] + rZ * kTiltSampleM);
    const float hL = TerrainAt(v.pos[0] - rX * kTiltSampleM, v.pos[2] - rZ * kTiltSampleM);
    v.pitch = std::atan2(hB - hF, 2.0f * kTiltSampleM); // nose up on uphill
    v.roll  = std::atan2(hR - hL, 2.0f * kTiltSampleM);
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
        dc->active = v.deployed;
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

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Aegis %u %s by player %u",
                   v->entity, wantDeploy ? "DEPLOYED" : "undeployed", player);
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

EntityId TFVehicleSystem::RaycastVehicles(const float origin[3], const float dir[3],
                                          float maxDist, float outHitPoint[3],
                                          float* outDist) const
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
    const FactionId attackerFaction =
        (m_ctx->players && attackerPlayer != kInvalidPlayer)
            ? m_ctx->players->FactionOf(attackerPlayer) : FactionId::None;
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
            msg.type = static_cast<Spark::Net::MessageType>(
                static_cast<uint16_t>(TFMsg::HitConfirm));
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

void TFVehicleSystem::ServerApplySplash(const float at[3], float radiusM, float damage,
                                        float vsVehicleMult, EntityId attackerPawn,
                                        PlayerId attackerPlayer, WeaponId weapon,
                                        EntityId excludeVehicle)
{
    if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || radiusM <= 0.0f || damage <= 0.0f)
        return;

    // Collect first, damage second: ServerDamageVehicle can destroy (erase)
    // records, which would invalidate iteration over m_vehicles.
    struct Hit { EntityId veh; float dmg; };
    std::vector<Hit> hits;
    for (const VehicleRec& v : m_vehicles)
    {
        if (v.hp <= 0.0f || v.entity == excludeVehicle)
            continue;
        const float d = std::sqrt(Dist2XZ(v.pos, at[0], at[2]) +
                                  (v.pos[1] - at[1]) * (v.pos[1] - at[1]));
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

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] vehicle %u (%s) destroyed by player %u",
                   entity, def ? def->name.c_str() : "?", destroyer);

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
            // 50% of the standard 500+500 pool (DESIGN §4); survivable when
            // healthy, harsh when already hurt — but never a guaranteed kill.
            if (m_ctx->damage)
                m_ctx->damage->ServerApplyDamage(pawn.entity, entity, kInvalidPlayer,
                                                 kTFVehEjectDamageFrac * 1000.0f,
                                                 kDamageKindExplosive, kInvalidWeapon, false);
        }
    }

    // 2) Explosion splash on other nearby pawns (kill credit -> destroyer).
    if (m_ctx->players && m_ctx->damage)
    {
        const float at[3] = {v.pos[0], v.pos[1] + 1.0f, v.pos[2]};
        std::vector<std::pair<EntityId, float>> hits;
        m_ctx->players->ForEachAlivePawn([&](const PawnInfo& pawn) {
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
            m_ctx->damage->ServerApplyDamage(pawnEntity, entity, destroyer, dmg,
                                             kDamageKindExplosive, kInvalidWeapon, false);
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

    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
    if (world && v.local != 0)
    {
        const auto e = static_cast<EntityID>(v.local);
        if (world->GetRegistry().valid(e))
            world->DestroyEntity(e);
    }
    m_lastSent.erase(entity);
    m_vehicles.erase(std::remove_if(m_vehicles.begin(), m_vehicles.end(),
                                    [entity](const VehicleRec& r) { return r.entity == entity; }),
                     m_vehicles.end());
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
            info.pos[0] = v.pos[0]; info.pos[1] = v.pos[1]; info.pos[2] = v.pos[2];
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
    ForEachVehicle([&](const TFVehicleInfo& info) {
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
