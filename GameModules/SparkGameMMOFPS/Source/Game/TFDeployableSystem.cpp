/**
 * @file TFDeployableSystem.cpp
 * @brief Fabricator turret / ammo pack + Medtech beacon (W3): server mechanics,
 *        0x54FC-0x54FE replication, client mirror + prop visuals.
 *
 * Tick driving: Main.cpp (frozen) routes only Update() to this system, so all
 * mechanics run on the render tick with dt accumulation — none of them need
 * fixed-step determinism (timers, radius checks, hitscan through TFDamageSystem).
 * FixedUpdate stays a contract no-op so a future Main wiring cannot double-tick.
 */
#include "Game/TFDeployableSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVisualUtils.h"
#include "Net/TFRepProtocol.h"
#include "World/TFWorldSetup.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace Terrafront {

namespace {

// --- W3 deployable stats (balance pass is W4; keep in one table) ------------
struct KindStats {
    float health, lifeSec;
};
constexpr KindStats kStats[] = {
    /* FabTurret  */ {300.0f, 60.0f},
    /* FabAmmoPack*/ {150.0f, 60.0f},
    /* MedBeacon  */ {150.0f, 45.0f},
};

constexpr float kPlaceAheadM      = 2.0f;    // drop point in front of the pawn
constexpr float kPlaceLiftM       = 0.05f;   // epsilon above terrain
constexpr float kTurretRangeM     = 40.0f;
constexpr float kTurretDamage     = 95.0f;
constexpr float kTurretRpm        = 450.0f;
constexpr float kTurretMuzzleY    = 1.5f;    // fire point above base
constexpr float kTurretAimY       = 1.2f;    // aim at pawn chest height
constexpr float kAmmoPackRadiusM  = 5.0f;
constexpr float kAmmoPackHealHp   = 20.0f;   // TF-W4: real ammo refill instead
constexpr float kAmmoPackPulseSec = 5.0f;
constexpr float kBeaconRadiusM    = 6.0f;
constexpr float kBeaconHealPerSec = 30.0f;
constexpr float kKeepaliveSec     = 1.0f;    // health/life refresh cadence
constexpr uint8_t kDamageKindBullet = 0;     // TF_DamageEvent convention

// Synthetic network ids when no ECS world exists (headless unit tests);
// distinct base from TFPlayerSystem's 1000000 pawn range.
EntityId g_nextSyntheticEntity = 2000000;

constexpr float kRadToDeg = 57.2957795f;

float Dist2(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

TFDeployableSystem::TFDeployableSystem() = default;
TFDeployableSystem::~TFDeployableSystem() { if (m_initialized) Shutdown(); }

bool TFDeployableSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDeployableSystem initialized");
    return true;
}

void TFDeployableSystem::Update(float deltaTime)
{
    if (!m_initialized || !m_ctx)
        return;
    m_clock += deltaTime;

    if (m_ctx->IsAuthority())
    {
        ServerTick(deltaTime);
    }
    else
    {
        // client mirror: decay lifetimes locally between server keepalives
        for (auto& [entity, rec] : m_deployables)
            rec.view.life = std::max(0.0f, rec.view.life - deltaTime);
    }

#ifdef ENABLE_NETWORKING
    if (ClientNetActive())
    {
        if (!m_clientHandlers)
            ClientEnsureHandlers();
    }
    else if (m_clientHandlers)
    {
        ClientReleaseHandlers();
        for (auto& [entity, rec] : m_deployables)
            DestroyLocalEntity(rec.local);
        m_deployables.clear();
    }
#endif
}

// Main.cpp does not drive FixedUpdate for this system; see file header.
void TFDeployableSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFDeployableSystem::Shutdown()
{
    if (!m_initialized)
        return;
#ifdef ENABLE_NETWORKING
    if (m_clientHandlers)
        ClientReleaseHandlers();
    m_knownClients.clear();
#endif
    for (auto& [entity, rec] : m_deployables)
        DestroyLocalEntity(rec.local);
    m_deployables.clear();
    m_ownerIndex.clear();
    m_initialized = false;
}

double TFDeployableSystem::NowSec() const { return m_clock; }

// ---------------------------------------------------------------------------
// Placement (server)
// ---------------------------------------------------------------------------

const char* TFDeployableSystem::ResultText(TFDeployResult r)
{
    switch (r)
    {
        case TFDeployResult::Ok:           return "deployed";
        case TFDeployResult::NotAuthority: return "server only";
        case TFDeployResult::DataMissing:  return "data tables not loaded";
        case TFDeployResult::NoPawn:       return "you must be alive";
        case TFDeployResult::WrongClass:   return "your class cannot place that";
        case TFDeployResult::BadKind:      return "unknown deployable kind";
        case TFDeployResult::SpawnFailed:  return "placement failed";
    }
    return "?";
}

TFDeployResult TFDeployableSystem::ServerTryPlaceDeployable(PlayerId player,
                                                            DeployableKind kind)
{
    if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
        return TFDeployResult::NotAuthority;
    if (static_cast<uint8_t>(kind) >= static_cast<uint8_t>(DeployableKind::COUNT))
        return TFDeployResult::BadKind;

    PawnInfo pawn{};
    if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
        return TFDeployResult::NoPawn;

    // Class gate: Fabricator tools vs Medtech beacon (DESIGN.md classes).
    const bool fabKind = kind == DeployableKind::FabTurret || kind == DeployableKind::FabAmmoPack;
    if ((fabKind && pawn.cls != ClassId::Fabricator) ||
        (!fabKind && pawn.cls != ClassId::Medtech))
        return TFDeployResult::WrongClass;

    // Max 1 per player per kind: placing again replaces the previous one.
    auto& byKind = m_ownerIndex[player];
    if (auto it = byKind.find(static_cast<uint8_t>(kind)); it != byKind.end())
        ServerDestroyDeployable(it->second, "replaced");

    TFDeployableView view{};
    view.kind      = kind;
    view.owner     = player;
    view.faction   = pawn.faction;
    view.yaw       = pawn.yaw;
    view.maxHealth = kStats[static_cast<uint8_t>(kind)].health;
    view.health    = view.maxHealth;
    view.life      = kStats[static_cast<uint8_t>(kind)].lifeSec;

    // Drop point: 2 m ahead on the pawn's facing (yaw convention: fwd = sin/cos,
    // see TFPlayerSystem::FindSkyanchorSpawn), clamped onto the terrain.
    view.pos[0] = pawn.pos[0] + std::sin(pawn.yaw) * kPlaceAheadM;
    view.pos[2] = pawn.pos[2] + std::cos(pawn.yaw) * kPlaceAheadM;
    view.pos[1] = (m_ctx->world ? m_ctx->world->TerrainHeightAt(view.pos[0], view.pos[2])
                                : pawn.pos[1]) + kPlaceLiftM;

    Rec rec{};
    rec.local = CreateDeployableEntity(view);
    view.entity = (rec.local != 0) ? rec.local : g_nextSyntheticEntity++;
    rec.view = view;
    rec.nextShotAt  = m_clock;
    rec.nextPulseAt = m_clock + kAmmoPackPulseSec;

    m_deployables[view.entity] = rec;
    byKind[static_cast<uint8_t>(kind)] = view.entity;
    ++m_placed;

#ifdef ENABLE_NETWORKING
    if (ServerNetActive())
        SendCreate(kInvalidPlayer, m_deployables[view.entity].view);
#endif

    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] deployable %u (kind %u) placed by player %u at (%.0f %.0f %.0f)",
                   view.entity, static_cast<unsigned>(kind), player,
                   view.pos[0], view.pos[1], view.pos[2]);
    return TFDeployResult::Ok;
}

// ---------------------------------------------------------------------------
// Server tick
// ---------------------------------------------------------------------------

void TFDeployableSystem::ServerTick(float dt)
{
#ifdef ENABLE_NETWORKING
    if (ServerNetActive())
        ServerPollNewClients();
    else
        m_knownClients.clear();
#endif

    // Lifetime + mechanics. Ticks only damage PAWNS (never this map), so
    // iteration is safe; expiries are collected and destroyed afterwards.
    std::vector<EntityId> expired;
    for (auto& [entity, rec] : m_deployables)
    {
        rec.view.life -= dt;
        if (rec.view.life <= 0.0f)
        {
            expired.push_back(entity);
            continue;
        }
        switch (rec.view.kind)
        {
            case DeployableKind::FabTurret:   TickTurret(rec);         break;
            case DeployableKind::FabAmmoPack: TickAmmoPack(rec);       break;
            case DeployableKind::MedBeacon:   TickMedBeacon(rec, dt);  break;
            default: break;
        }
    }
    for (EntityId e : expired)
    {
        ++m_expired;
        ServerDestroyDeployable(e, "expired");
    }

#ifdef ENABLE_NETWORKING
    // Slow keepalive so client health/life mirrors cannot drift for long.
    m_keepaliveAccum += dt;
    if (m_keepaliveAccum >= kKeepaliveSec)
    {
        m_keepaliveAccum = 0.0f;
        if (ServerNetActive())
            for (const auto& [entity, rec] : m_deployables)
                SendUpdate(rec.view);
    }
#endif
}

bool TFDeployableSystem::TurretHasLoS(const float from[3], const float to[3]) const
{
    if (!m_ctx || !m_ctx->world)
        return true;
    // 1 m terrain sampling, same approach as TFWeaponServer::TerrainBlocked.
    const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len <= 1.0f)
        return true;
    const float inv = 1.0f / len;
    for (float t = 1.0f; t < len; t += 1.0f)
    {
        const float x = from[0] + d[0] * inv * t;
        const float y = from[1] + d[1] * inv * t;
        const float z = from[2] + d[2] * inv * t;
        if (y < m_ctx->world->TerrainHeightAt(x, z))
            return false;
    }
    return true;
}

void TFDeployableSystem::TickTurret(Rec& rec)
{
    if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextShotAt)
        return;

    const float muzzle[3] = {rec.view.pos[0], rec.view.pos[1] + kTurretMuzzleY,
                             rec.view.pos[2]};

    // (Re)acquire: nearest alive enemy pawn in range with terrain LoS. The
    // current target is naturally revalidated because it competes as nearest.
    const float range2 = kTurretRangeM * kTurretRangeM;
    EntityId best = 0;
    float bestD2 = range2;
    m_ctx->players->ForEachAlivePawn([&](const PawnInfo& p) {
        if (p.faction == rec.view.faction || p.faction == FactionId::None)
            return;
        const float aim[3] = {p.pos[0], p.pos[1] + kTurretAimY, p.pos[2]};
        const float d2 = Dist2(rec.view.pos, p.pos);
        if (d2 > bestD2 || !TurretHasLoS(muzzle, aim))
            return;
        best = p.entity;
        bestD2 = d2;
    });
    rec.targetPawn = best;
    if (best == 0)
        return;

    // Lazy killfeed weapon id: dedicated "fab_turret" row if the data agent
    // adds one; falls back to "-" in the killfeed (TF-W4: own weapon row).
    if (m_turretWeapon == kInvalidWeapon && m_ctx->data && m_ctx->data->IsLoaded())
        if (const WeaponDef* wd = m_ctx->data->GetWeaponByKey("fab_turret"))
            m_turretWeapon = wd->id;

    // Instant hitscan through the damage pipeline: OWNER gets kill credit +
    // hitmarker; the turret entity is the attacker pawn for direction UI.
    m_ctx->damage->ServerApplyDamage(best, rec.view.entity, rec.view.owner,
                                     kTurretDamage, kDamageKindBullet,
                                     m_turretWeapon, false);
    ++m_turretShots;
    rec.nextShotAt = m_clock + 60.0 / kTurretRpm; // tracers/audio are TF-W4
}

void TFDeployableSystem::TickAmmoPack(Rec& rec)
{
    if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextPulseAt)
        return;
    rec.nextPulseAt = m_clock + kAmmoPackPulseSec;

    // W3 stand-in effect: small heal pulse to same-faction pawns in 5 m.
    // TF-W4: real mag/reserve refill (server ShooterState + client ammo sync).
    const float r2 = kAmmoPackRadiusM * kAmmoPackRadiusM;
    m_ctx->players->ForEachAlivePawn([&](const PawnInfo& p) {
        if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
            return;
        m_ctx->damage->ServerHeal(p.entity, kAmmoPackHealHp);
        m_healGiven += kAmmoPackHealHp;
    });
}

void TFDeployableSystem::TickMedBeacon(Rec& rec, float dt)
{
    if (!m_ctx->players || !m_ctx->damage)
        return;
    const float r2 = kBeaconRadiusM * kBeaconRadiusM;
    const float amount = kBeaconHealPerSec * dt;
    m_ctx->players->ForEachAlivePawn([&](const PawnInfo& p) {
        if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
            return;
        m_ctx->damage->ServerHeal(p.entity, amount); // clamps, never revives
        m_healGiven += amount;
    });
}

// ---------------------------------------------------------------------------
// Damage / destroy (server)
// ---------------------------------------------------------------------------

void TFDeployableSystem::ServerDamageDeployable(EntityId deployable,
                                                PlayerId attackerPlayer, float amount)
{
    if (!m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
        return;
    auto it = m_deployables.find(deployable);
    if (it == m_deployables.end())
        return;
    Rec& rec = it->second;

    // Same-faction fire is ignored (cheap objects; W4 revisits grief policy).
    if (m_ctx->players && attackerPlayer != kInvalidPlayer &&
        m_ctx->players->FactionOf(attackerPlayer) == rec.view.faction)
        return;

    rec.view.health -= amount;
    if (rec.view.health > 0.0f)
    {
#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            SendUpdate(rec.view);
#endif
        return;
    }
    ++m_destroyedByDamage;
    ServerDestroyDeployable(deployable, "destroyed");
}

void TFDeployableSystem::ServerSplashDamageDeployables(const float at[3], float radiusM,
                                                       float damage, PlayerId attackerPlayer)
{
    if (!m_ctx || !m_ctx->IsAuthority() || radiusM <= 0.0f || damage <= 0.0f)
        return;

    // Collect first — ServerDamageDeployable may erase entries.
    std::vector<std::pair<EntityId, float>> hits;
    for (const auto& [entity, rec] : m_deployables)
    {
        const float d = std::sqrt(Dist2(rec.view.pos, at));
        if (d > radiusM)
            continue;
        const float dmg = damage * (1.0f - d / radiusM); // pawn splash falloff
        if (dmg > 1.0f)
            hits.emplace_back(entity, dmg);
    }
    for (const auto& [entity, dmg] : hits)
        ServerDamageDeployable(entity, attackerPlayer, dmg);
}

void TFDeployableSystem::ServerDestroyDeployable(EntityId entity, const char* why)
{
    auto it = m_deployables.find(entity);
    if (it == m_deployables.end())
        return;
    Rec& rec = it->second;

#ifdef ENABLE_NETWORKING
    if (ServerNetActive())
        SendDestroy(entity);
#endif

    auto oit = m_ownerIndex.find(rec.view.owner);
    if (oit != m_ownerIndex.end())
    {
        auto kit = oit->second.find(static_cast<uint8_t>(rec.view.kind));
        if (kit != oit->second.end() && kit->second == entity)
            oit->second.erase(kit);
    }

    DestroyLocalEntity(rec.local);
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] deployable %u removed (%s)", entity, why);
    m_deployables.erase(it);
}

// ---------------------------------------------------------------------------
// Entities & visuals (both sides)
// ---------------------------------------------------------------------------

uint32_t TFDeployableSystem::CreateDeployableEntity(const TFDeployableView& view)
{
    World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
    if (!world)
        return 0;

    EntityID e = world->CreateEntity("TF_Deploy_K" + std::to_string(
                                         static_cast<unsigned>(view.kind)));
    if (static_cast<uint32_t>(e) == 0) // id 0 is reserved (same dodge as pawns)
        e = world->CreateEntity("TF_Deploy_K" + std::to_string(
                                    static_cast<unsigned>(view.kind)));

    Transform& t = world->AddComponent<Transform>(e);
    t.position = {view.pos[0], view.pos[1], view.pos[2]};
    t.rotation.y = view.yaw * kRadToDeg;

    NetworkIdentity& ni = world->AddComponent<NetworkIdentity>(e);
    ni.networkID = (view.entity != 0) ? view.entity : static_cast<uint32_t>(e);
    ni.ownerClientID = view.owner;
    ni.isLocalAuthority = m_ctx->IsAuthority();

    TFDeployableComp& dc = world->AddComponent<TFDeployableComp>(e);
    dc.kind = view.kind;
    dc.owner = view.owner;
    dc.life = view.life;

    world->AddComponent<TFFactionComp>(e).faction = view.faction;

    AttachDeployableVisual(static_cast<uint32_t>(e), view.kind, view.faction);
    return static_cast<uint32_t>(e);
}

void TFDeployableSystem::DestroyLocalEntity(uint32_t local)
{
    if (local == 0)
        return;
    World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
    const auto e = static_cast<EntityID>(local);
    if (world && world->GetRegistry().valid(e))
        world->DestroyEntity(e);
}

void TFDeployableSystem::AttachDeployableVisual(uint32_t local, DeployableKind kind,
                                                FactionId faction)
{
    World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
    const auto e = static_cast<EntityID>(local);
    if (!world || !world->GetRegistry().valid(e) || world->HasComponent<MeshRenderer>(e))
        return;

    // Prop stand-ins (Assets/Models/MMOFPS/props, W1 asset staging): turret =
    // antenna mast, ammo pack = crate, med beacon = slim antenna (data-driven
    // via Assets/MMOFPS/Data/deployables.json). Faction is telegraphed by
    // material like pawn visuals; bespoke meshes are TF-W4.
    const DeployableVisualDef* dv = (m_ctx->data && m_ctx->data->IsLoaded())
                                         ? m_ctx->data->GetDeployableVisual(kind)
                                         : nullptr;
    if (!dv)
        return; // no data table loaded (e.g. headless harness) - nothing to draw

    MeshRenderer& mr = world->AddComponent<MeshRenderer>(e);
    mr.meshPath = "Assets/" + dv->model;
    mr.materialPath = FactionStructureMaterial(*m_ctx, faction);
    mr.castShadows = true;

    if (Transform* t = world->GetComponent<Transform>(e))
        t->scale = {dv->scale[0], dv->scale[1], dv->scale[2]};
}

// ---------------------------------------------------------------------------
// Queries (both sides)
// ---------------------------------------------------------------------------

bool TFDeployableSystem::GetDeployable(EntityId entity, TFDeployableView& out) const
{
    auto it = m_deployables.find(entity);
    if (it == m_deployables.end())
        return false;
    out = it->second.view;
    return true;
}

void TFDeployableSystem::ForEachDeployable(
    const std::function<void(const TFDeployableView&)>& fn) const
{
    for (const auto& [entity, rec] : m_deployables)
        fn(rec.view);
}

#ifdef ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Replication — server broadcast (patterns mirror TFReplication.cpp)
// ---------------------------------------------------------------------------

bool TFDeployableSystem::ServerNetActive() const
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    return nm.IsInitialized() &&
           nm.GetRole() == Spark::Net::NetworkRole::Server &&
           m_ctx->IsAuthority();
}

bool TFDeployableSystem::ClientNetActive() const
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    return nm.IsInitialized() &&
           nm.GetRole() == Spark::Net::NetworkRole::Client;
}

void TFDeployableSystem::ServerPollNewClients()
{
    // No join callback slot on NetworkManager — diff GetClients() like
    // TFServerSim/TFReplication so late joiners get the current set.
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    const auto& clients = nm.GetClients();

    for (const auto& [id, info] : clients)
    {
        if (info.state != Spark::Net::ConnectionState::Connected ||
            m_knownClients.contains(id))
            continue;
        m_knownClients.insert(id);
        for (const auto& [entity, rec] : m_deployables)
            SendCreate(id, rec.view);
    }
    for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
    {
        if (!clients.contains(*it))
            it = m_knownClients.erase(it);
        else
            ++it;
    }
}

void TFDeployableSystem::SendCreate(PlayerId target, const TFDeployableView& view)
{
    TF_RepDeployCreate c{};
    c.entityId    = view.entity;
    c.ownerPlayer = view.owner;
    c.kind        = static_cast<uint8_t>(view.kind);
    c.faction     = static_cast<uint8_t>(view.faction);
    c.posX = view.pos[0]; c.posY = view.pos[1]; c.posZ = view.pos[2];
    c.yaw = view.yaw;
    c.health = view.health; c.maxHealth = view.maxHealth;
    c.lifeSec = view.life;
    SendRep(target, kTFRepMsg_DeployCreate, &c, sizeof(c), true);
}

void TFDeployableSystem::SendUpdate(const TFDeployableView& view)
{
    TF_RepDeployUpdate u{};
    u.entityId = view.entity;
    u.health = view.health;
    u.lifeSec = view.life;
    SendRep(kInvalidPlayer, kTFRepMsg_DeployUpdate, &u, sizeof(u), false);
}

void TFDeployableSystem::SendDestroy(EntityId entity)
{
    TF_RepDeployDestroy d{entity};
    SendRep(kInvalidPlayer, kTFRepMsg_DeployDestroy, &d, sizeof(d), true);
}

void TFDeployableSystem::SendRep(PlayerId target, uint16_t msgId, const void* payload,
                                 size_t size, bool reliable)
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    Spark::Net::NetworkMessage msg;
    msg.type = static_cast<Spark::Net::MessageType>(msgId);
    msg.channel = reliable ? Spark::Net::ChannelType::Reliable
                           : Spark::Net::ChannelType::Unreliable;
    msg.payload.resize(size);
    std::memcpy(msg.payload.data(), payload, size);
    if (target == kInvalidPlayer)
        nm.SendToAll(msg);
    else
        nm.SendToClient(target, msg);
}

// ---------------------------------------------------------------------------
// Replication — client mirror
// ---------------------------------------------------------------------------

void TFDeployableSystem::ClientEnsureHandlers()
{
    using Spark::Net::MessageType;
    using Spark::Net::NetworkMessage;
    auto& nm = Spark::Net::NetworkManager::GetInstance();

    nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployCreate),
                       [this](const NetworkMessage& m) { OnDeployCreate(m.payload.data(), m.payload.size()); });
    nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployUpdate),
                       [this](const NetworkMessage& m) { OnDeployUpdate(m.payload.data(), m.payload.size()); });
    nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployDestroy),
                       [this](const NetworkMessage& m) { OnDeployDestroy(m.payload.data(), m.payload.size()); });
    m_clientHandlers = true;
}

void TFDeployableSystem::ClientReleaseHandlers()
{
    // No per-type removal on NetworkManager — swap in no-ops so no dangling
    // `this` survives shutdown (established TFReplication pattern).
    using Spark::Net::MessageType;
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    for (uint16_t id : {kTFRepMsg_DeployCreate, kTFRepMsg_DeployUpdate,
                        kTFRepMsg_DeployDestroy})
        nm.RegisterHandler(static_cast<MessageType>(id),
                           [](const Spark::Net::NetworkMessage&) {});
    m_clientHandlers = false;
}

void TFDeployableSystem::OnDeployCreate(const void* data, size_t size)
{
    if (size != sizeof(TF_RepDeployCreate))
        return;
    TF_RepDeployCreate c;
    std::memcpy(&c, data, sizeof(c));

    Rec& rec = m_deployables[c.entityId]; // upsert (re-create keeps the visual)
    rec.view.entity    = c.entityId;
    rec.view.kind      = static_cast<DeployableKind>(c.kind);
    rec.view.owner     = c.ownerPlayer;
    rec.view.faction   = static_cast<FactionId>(c.faction);
    rec.view.pos[0] = c.posX; rec.view.pos[1] = c.posY; rec.view.pos[2] = c.posZ;
    rec.view.yaw       = c.yaw;
    rec.view.health    = c.health;
    rec.view.maxHealth = c.maxHealth;
    rec.view.life      = c.lifeSec;
    if (rec.local == 0)
        rec.local = CreateDeployableEntity(rec.view);
}

void TFDeployableSystem::OnDeployUpdate(const void* data, size_t size)
{
    if (size != sizeof(TF_RepDeployUpdate))
        return;
    TF_RepDeployUpdate u;
    std::memcpy(&u, data, sizeof(u));
    auto it = m_deployables.find(u.entityId);
    if (it == m_deployables.end())
        return; // unreliable update raced the reliable Create — skip
    it->second.view.health = u.health;
    it->second.view.life   = u.lifeSec;
}

void TFDeployableSystem::OnDeployDestroy(const void* data, size_t size)
{
    if (size != sizeof(TF_RepDeployDestroy))
        return;
    TF_RepDeployDestroy d;
    std::memcpy(&d, data, sizeof(d));
    auto it = m_deployables.find(d.entityId);
    if (it == m_deployables.end())
        return;
    DestroyLocalEntity(it->second.local);
    m_deployables.erase(it);
}

#endif // ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Debug UI
// ---------------------------------------------------------------------------

void TFDeployableSystem::RenderDebugUI()
{
#ifdef SPARK_HAS_IMGUI
    if (!ImGui::CollapsingHeader("TF Deployables"))
        return;
    ImGui::Text("live: %zu  placed: %u  expired: %u  killed: %u",
                m_deployables.size(), m_placed, m_expired, m_destroyedByDamage);
    ImGui::Text("turret shots: %u  healing given: %.0f hp", m_turretShots, m_healGiven);
    for (const auto& [entity, rec] : m_deployables)
    {
        static const char* kKindName[] = {"Turret", "AmmoPack", "MedBeacon"};
        const uint8_t k = static_cast<uint8_t>(rec.view.kind);
        ImGui::Text("e%u %s p%u %s hp=%.0f/%.0f life=%.0fs pos=(%.0f %.0f %.0f)",
                    entity, k < 3 ? kKindName[k] : "?", rec.view.owner,
                    FactionTag(rec.view.faction), rec.view.health, rec.view.maxHealth,
                    rec.view.life, rec.view.pos[0], rec.view.pos[1], rec.view.pos[2]);
    }
#endif
}

} // namespace Terrafront
