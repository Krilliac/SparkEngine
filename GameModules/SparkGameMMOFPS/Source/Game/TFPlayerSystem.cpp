/**
 * @file TFPlayerSystem.cpp
 * @brief Pawn registry + server spawn/kill/respawn flow, W1 full ECS
 *        implementation.
 *
 * Authority: pawns are real ECS entities (Transform + HealthComponent +
 * NetworkIdentity + TF components from Game/TFComponents.h). TFServerSim
 * integrates movement and writes the pawn Transform each fixed tick
 * (position = feet, rotation.y = yaw in degrees) — PawnInfo reads that
 * Transform as the position/yaw truth; velocity/pitch come from
 * TFPawnMoveComp when a writer fills it (zeros until then).
 *
 * Query surface + the pure-client half (RemotePawn discovery, visuals,
 * debug panel) live in TFPlayerSystemClient.cpp (same class, split per
 * repo file-size rules).
 */
#include "Game/TFPlayerSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFVehicleSystem.h"   // W3 shared-edit: Aegis mobile-spawn (spawnKind==2)
#include "Net/TFServerSim.h"
#include "UI/TFHUD.h"
#include "World/TFWorldSetup.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace Terrafront {

namespace {

constexpr float kRadToDeg   = 57.2957795f;
constexpr float kSpawnLiftM = 0.10f;   // spawn epsilon above the terrain
constexpr float kMapCenter  = 2048.0f; // face the middle of Cindral Wastes

// Synthetic network-entity ids used only when no ECS world exists (headless
// unit tests without an engine world). Real builds always take the ECS path.
EntityId g_nextSyntheticEntity = 1000000;

} // namespace

TFPlayerSystem::TFPlayerSystem() = default;
TFPlayerSystem::~TFPlayerSystem() { if (m_initialized) Shutdown(); }

bool TFPlayerSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFPlayerSystem initialized");
    return true;
}

void TFPlayerSystem::Update(float deltaTime)
{
    if (!m_initialized || !m_ctx)
        return;
    m_clock += deltaTime;

    if (m_ctx->IsAuthority())
        UpdateAuthorityLifecycle();
    else
        SyncClientRecords();
}

void TFPlayerSystem::FixedUpdate(float) {}

void TFPlayerSystem::Shutdown()
{
    for (auto& [player, rec] : m_players)
        DestroyPawn(rec);
    m_players.clear();
    m_pawnOwner.clear();
    m_pendingLocalPawn = 0;
    m_initialized = false;
}

double TFPlayerSystem::NowSec() const
{
    return (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
}

// ---------------------------------------------------------------- server ops

uint32_t TFPlayerSystem::CreatePawnEntity(PlayerId player, FactionId faction, ClassId cls,
                                          const float pos[3], float yaw)
{
    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
    if (!world)
        return 0;

    EntityID e = world->CreateEntity("TF_Pawn_P" + std::to_string(player));
    if (static_cast<uint32_t>(e) == 0)
    {
        // Entity id 0 means "no pawn" in the frozen W1 contract — park it on a
        // never-destroyed placeholder and take the next id for the pawn.
        e = world->CreateEntity("TF_Pawn_P" + std::to_string(player));
    }

    Transform& t = world->AddComponent<Transform>(e);
    t.position = {pos[0], pos[1], pos[2]};
    t.rotation.y = yaw * kRadToDeg;

    const ClassDef* cd = (m_ctx->data && m_ctx->data->IsLoaded())
                             ? m_ctx->data->GetClass(cls) : nullptr;
    const float maxHealth = cd ? cd->health : 500.0f;
    const float maxShield = cd ? cd->shield : 500.0f;

    HealthComponent& hc = world->AddComponent<HealthComponent>(e);
    hc.health = maxHealth;
    hc.maxHealth = maxHealth;

    NetworkIdentity& ni = world->AddComponent<NetworkIdentity>(e);
    ni.networkID = static_cast<uint32_t>(e);
    ni.ownerClientID = player;
    ni.isLocalAuthority = true; // this process is the simulation authority

    world->AddComponent<TFFactionComp>(e).faction = faction;
    world->AddComponent<TFClassComp>(e).cls = cls;
    world->AddComponent<TFPawnComp>(e).owner = player;

    TFShieldComp& sh = world->AddComponent<TFShieldComp>(e);
    sh.cur = maxShield;
    sh.max = maxShield;
    if (const FactionDef* fd = (m_ctx->data && m_ctx->data->IsLoaded())
                                   ? m_ctx->data->GetFaction(faction) : nullptr)
        sh.regenDelay = fd->shieldRegenDelaySec;

    TFPawnMoveComp& mv = world->AddComponent<TFPawnMoveComp>(e);
    mv.yaw = yaw;

    TFWeaponHeldComp& wh = world->AddComponent<TFWeaponHeldComp>(e);
    wh.weaponId = PickDefaultWeapon(faction, cls);
    if (const WeaponDef* wd = (wh.weaponId != kInvalidWeapon && m_ctx->data)
                                  ? m_ctx->data->GetWeapon(wh.weaponId) : nullptr)
    {
        wh.ammoMag = wd->magSize;
        wh.ammoPool = wd->reserve;
    }

    // First-person local pawns render no body (W1; TF-W4: shadow-only body).
    const bool isLocalFirstPerson =
        m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer;
    if (!isLocalFirstPerson)
        AttachPawnVisual(static_cast<uint32_t>(e), faction);

    return static_cast<uint32_t>(e);
}

EntityId TFPlayerSystem::ServerSpawnPawn(PlayerId player, FactionId faction, ClassId cls,
                                         const float pos[3], float yaw)
{
    if (!m_ctx || !m_ctx->IsAuthority())
        return 0;

    PlayerRec& rec = m_players[player];
    if (rec.hasPawn)
        DestroyPawn(rec);

    const uint32_t local = CreatePawnEntity(player, faction, cls, pos, yaw);

    rec.faction = faction;
    rec.cls     = cls;
    rec.local   = local;
    rec.pawn    = (local != 0) ? local : g_nextSyntheticEntity++;
    rec.hasPawn = true;
    rec.alive   = true;
    rec.nextRespawnAt = 0.0;
    rec.despawnAt     = 0.0;
    m_pawnOwner[rec.pawn] = player;

    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] pawn %u spawned for player %u (%s class %u) at (%.0f %.0f %.0f)",
                   rec.pawn, player, FactionTag(faction), static_cast<unsigned>(cls),
                   pos[0], pos[1], pos[2]);

    if (m_events)
        m_events->Fire(EvPlayerSpawned{player, rec.pawn, cls, faction});
    return rec.pawn;
}

void TFPlayerSystem::ServerKillPawn(EntityId victim, PlayerId killerPlayer,
                                    WeaponId weapon, bool headshot)
{
    auto it = m_pawnOwner.find(victim);
    if (it == m_pawnOwner.end())
        return;
    const PlayerId victimPlayer = it->second;
    PlayerRec& rec = m_players[victimPlayer];
    if (!rec.alive)
        return;

    rec.alive = false;
    rec.nextRespawnAt = NowSec() + kTFRespawnDelaySec;
    rec.despawnAt     = NowSec() + kTFDespawnDelaySec;

    ServerSetPawnHealth(victim, 0.0f, 0.0f);

    if (m_events)
    {
        m_events->Fire(EvPlayerKilled{victimPlayer, killerPlayer, weapon, headshot});
        if (m_ctx->HasLocalPlayer() && victimPlayer == m_ctx->localPlayer)
            m_events->Fire(EvLocalPlayerDied{victimPlayer, kTFRespawnDelaySec});
    }
}

void TFPlayerSystem::ServerSetPawnHealth(EntityId entity, float health, float shield)
{
    uint32_t local = 0;
    if (!ResolveEntity(entity, local))
        return;
    World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
    const auto e = static_cast<EntityID>(local);
    if (!world || !world->GetRegistry().valid(e))
        return;

    if (HealthComponent* hc = world->GetComponent<HealthComponent>(e))
    {
        hc->health = std::max(0.0f, health);
        hc->isDead = hc->health <= 0.0f;
    }
    if (TFShieldComp* sc = world->GetComponent<TFShieldComp>(e))
    {
        sc->cur = std::clamp(shield, 0.0f, sc->max);
        sc->sinceDamage = 0.0f;
    }
}

void TFPlayerSystem::ServerHandleFactionSelect(PlayerId player, FactionId faction)
{
    m_players[player].faction = faction;
}

void TFPlayerSystem::ServerHandleSpawnRequest(PlayerId player, const TF_SpawnRequest& req)
{
    PlayerRec& rec = m_players[player];

    // Faction can arrive via ServerHandleFactionSelect or TFServerSim's
    // registry (network FactionSelect routing) — accept either.
    FactionId faction = rec.faction;
    if (faction == FactionId::None && m_ctx->serverSim)
        faction = m_ctx->serverSim->GetPlayerFaction(player);

    TF_SpawnReply reply{};
    reply.accepted = 0;
    float pos[3] = {0, 0, 0};
    float yaw = 0.0f;

    if (faction == FactionId::None)
    {
        reply.reason = 1; // must pick a faction first
    }
    else if (req.classId >= static_cast<uint8_t>(ClassId::COUNT) ||
             req.classId == static_cast<uint8_t>(ClassId::Colossus))
    {
        reply.reason = 4; // class-locked (Colossus is terminal-purchased)
    }
    // W3 shared-edit (vehicles agent): spawnKind==2 (deployed friendly Aegis)
    // is now a first-class spawn point. Its respawn timer is shorter (DESIGN
    // §4: 5 s at an Aegis vs the 8 s default, data-driven via
    // vehicles.json deployRespawnSec).
    else if (req.spawnKind != 0 && req.spawnKind != 2)
    {
        reply.reason = 1; // TF-W2: region spawns; TF-W3 (squad agent): squad spawns
    }
    else if (req.spawnKind == 2 && !m_ctx->vehicles)
    {
        reply.reason = 1; // vehicles system absent (headless unit tests)
    }
    else if (rec.alive)
    {
        reply.reason = 1; // already deployed
    }
    else if (const double respawnAt =
                 rec.nextRespawnAt -
                 ((req.spawnKind == 2 && m_ctx->vehicles)
                      ? std::max(0.0f, kTFRespawnDelaySec - m_ctx->vehicles->AegisRespawnDelaySec())
                      : 0.0f);
             NowSec() < respawnAt)
    {
        reply.reason = 2; // respawn timer
        reply.respawnDelay = static_cast<float>(respawnAt - NowSec());
    }
    else if (req.spawnKind == 2
                 ? !m_ctx->vehicles->GetAegisSpawnPos(req.aegisEntity, faction, pos)
                 : !FindSkyanchorSpawn(faction, pos, yaw))
    {
        reply.reason = req.spawnKind == 2 ? 3 : 1; // 3 = aegis gone/undeployed/contested
    }
    else
    {
        rec.faction = faction;
        const EntityId pawn =
            ServerSpawnPawn(player, faction, static_cast<ClassId>(req.classId), pos, yaw);
        if (pawn != 0)
        {
            reply.accepted = 1;
            reply.reason = 0;
            reply.entityId = pawn;
            reply.posX = pos[0]; reply.posY = pos[1]; reply.posZ = pos[2];
        }
    }
    SendSpawnReply(player, reply);
}

void TFPlayerSystem::ServerHandlePlayerDisconnect(PlayerId player)
{
    auto it = m_players.find(player);
    if (it == m_players.end())
        return;
    DestroyPawn(it->second);
    m_players.erase(it);
}

void TFPlayerSystem::UpdateAuthorityLifecycle()
{
    // Corpse despawn: dead pawns keep their entity (visible corpse; the
    // replication destroy already went out on kill) until the hide window ends.
    const double now = NowSec();
    for (auto& [player, rec] : m_players)
    {
        if (rec.hasPawn && !rec.alive && rec.despawnAt > 0.0 && now >= rec.despawnAt)
            DestroyPawn(rec);
    }
}

// ---------------------------------------------------------------- helpers

void TFPlayerSystem::SendSpawnReply(PlayerId player, const TF_SpawnReply& reply)
{
    // In-process local player (listen host / standalone): no socket round-trip.
    if (m_ctx && m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer)
    {
        ClientOnSpawnReply(reply); // no-op on the authority by contract
        if (m_ctx->hud)
        {
            if (reply.accepted)
                m_ctx->hud->SetRespawnState(false, 0.0f);
            else if (reply.reason == 2)
                m_ctx->hud->SetRespawnState(true, reply.respawnDelay);
        }
        return;
    }

#ifdef ENABLE_NETWORKING
    if (m_ctx && m_ctx->role != NetRole::Standalone)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized())
        {
            Spark::Net::NetworkMessage msg;
            msg.type = static_cast<Spark::Net::MessageType>(
                static_cast<uint16_t>(TFMsg::SpawnReply));
            msg.channel = Spark::Net::ChannelType::Reliable;
            msg.payload.resize(sizeof(reply));
            std::memcpy(msg.payload.data(), &reply, sizeof(reply));
            nm.SendToClient(player, msg);
        }
    }
#endif
}

WeaponId TFPlayerSystem::PickDefaultWeapon(FactionId faction, ClassId cls) const
{
    if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
        return kInvalidWeapon;
    const ClassDef* cd = m_ctx->data->GetClass(cls);
    if (!cd || cd->primarySlots.empty())
        return kInvalidWeapon;
    for (const auto& w : m_ctx->data->AllWeapons())
        if (w.slot == cd->primarySlots.front() &&
            (w.faction == faction || w.faction == FactionId::None))
            return w.id;
    return kInvalidWeapon;
}

bool TFPlayerSystem::FindSkyanchorSpawn(FactionId faction, float outPos[3], float& outYaw) const
{
    if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
        return false;
    for (const auto& r : m_ctx->data->GetContinent().regions)
    {
        if (r.tier == "skyanchor" && r.homeFaction == faction)
        {
            const float x = r.spawns.empty() ? r.centerX : r.spawns.front()[0];
            const float z = r.spawns.empty() ? r.centerZ : r.spawns.front()[1];
            outPos[0] = x;
            outPos[1] = (m_ctx->world ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f) + kSpawnLiftM;
            outPos[2] = z;
            outYaw = std::atan2(kMapCenter - x, kMapCenter - z); // face map center
            return true;
        }
    }
    return false;
}

void TFPlayerSystem::DestroyPawn(PlayerRec& rec)
{
    if (!rec.hasPawn)
        return;

    if (rec.local != 0)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        const auto e = static_cast<EntityID>(rec.local);
        if (world && world->GetRegistry().valid(e))
            world->DestroyEntity(e);
    }

    m_pawnOwner.erase(rec.pawn);
    rec.hasPawn = false;
    rec.alive = false;
    rec.pawn = 0;
    rec.local = 0;
    rec.despawnAt = 0.0;
}

} // namespace Terrafront
