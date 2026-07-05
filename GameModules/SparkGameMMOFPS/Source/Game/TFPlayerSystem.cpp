/**
 * @file TFPlayerSystem.cpp
 * @brief Pawn registry + spawn/kill flow. (W1 minimal contract implementation:
 *        registry-backed pawns without ECS entity creation. The client-player
 *        agent replaces this with the full ECS/replication-integrated body —
 *        see TF-W1-FULL markers.)
 */
#include "Game/TFPlayerSystem.h"
#include "Data/TFDataTables.h"
#include "Net/TFClientNet.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstring>

namespace Terrafront {

namespace {
// Synthetic network-entity ids for registry-only pawns (TF-W1-FULL: real ECS ids).
EntityId g_nextPawnEntity = 1000;
}

TFPlayerSystem::TFPlayerSystem() = default;
TFPlayerSystem::~TFPlayerSystem() { if (m_initialized) Shutdown(); }

bool TFPlayerSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;
    m_initialized = true;
    return true;
}

void TFPlayerSystem::Update(float) {}
void TFPlayerSystem::FixedUpdate(float) {}

void TFPlayerSystem::Shutdown()
{
    m_players.clear();
    m_pawnOwner.clear();
    m_initialized = false;
}

void TFPlayerSystem::RenderDebugUI() {}

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
    // TF-W1-FULL: positions/velocity/view/health come from the pawn's ECS
    // components; the registry-only stub reports zeros.
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
    if (pit == m_players.end())
        return false;
    outLocalEntity = pit->second.local;
    return true;
}

// ---------------------------------------------------------------- server ops

EntityId TFPlayerSystem::ServerSpawnPawn(PlayerId player, FactionId faction, ClassId cls,
                                         const float pos[3], float yaw)
{
    (void)pos; (void)yaw;   // TF-W1-FULL: create ECS entity at pos/yaw + replication
    PlayerRec& rec = m_players[player];
    if (rec.hasPawn)
        DestroyPawn(rec);

    rec.faction = faction;
    rec.cls     = cls;
    rec.pawn    = g_nextPawnEntity++;
    rec.local   = 0;
    rec.hasPawn = true;
    rec.alive   = true;
    m_pawnOwner[rec.pawn] = player;

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
    PlayerRec& rec = m_players[it->second];
    rec.alive = false;
    if (m_events)
        m_events->Fire(EvPlayerKilled{it->second, killerPlayer, weapon, headshot});
}

void TFPlayerSystem::ServerSetPawnHealth(EntityId entity, float health, float shield)
{
    (void)entity; (void)health; (void)shield;
    // TF-W1-FULL: write HealthComponent/TFShieldComp on the pawn entity.
}

void TFPlayerSystem::ServerHandleFactionSelect(PlayerId player, FactionId faction)
{
    m_players[player].faction = faction;
}

void TFPlayerSystem::ServerHandleSpawnRequest(PlayerId player, const TF_SpawnRequest& req)
{
    PlayerRec& rec = m_players[player];

    TF_SpawnReply reply{};
    float pos[3] = {0, 0, 0};
    float yaw = 0;

    if (rec.faction == FactionId::None)
    {
        reply.accepted = 0;
        reply.reason = 4; // class/faction-locked
    }
    else if (req.spawnKind != 0 || !FindSkyanchorSpawn(rec.faction, pos, yaw))
    {
        reply.accepted = 0;
        reply.reason = 1; // bad point (W1: skyanchor only)
    }
    else
    {
        const EntityId pawn = ServerSpawnPawn(player, rec.faction,
                                              static_cast<ClassId>(req.classId), pos, yaw);
        reply.accepted = 1;
        reply.entityId = pawn;
        reply.posX = pos[0]; reply.posY = pos[1]; reply.posZ = pos[2];
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

// ---------------------------------------------------------------- client ops

void TFPlayerSystem::ClientOnSpawnReply(const TF_SpawnReply& reply)
{
    if (!m_ctx || m_ctx->IsAuthority() || !reply.accepted)
        return;
    m_pendingLocalPawn = reply.entityId;
    // TF-W1-FULL: associate with the replicated entity once it arrives.
}

// ---------------------------------------------------------------- helpers

void TFPlayerSystem::SendSpawnReply(PlayerId player, const TF_SpawnReply& reply)
{
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
            return;
        }
    }
#endif
    // Standalone / listen-host local player: loop straight back.
    ClientOnSpawnReply(reply);
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
        if (r.tier == "skyanchor" && r.homeFaction == faction && !r.spawns.empty())
        {
            outPos[0] = r.spawns.front()[0];
            outPos[1] = 0.0f;   // TF-W1-FULL: ctx.world->TerrainHeightAt
            outPos[2] = r.spawns.front()[1];
            outYaw = 0.0f;
            return true;
        }
    }
    return false;
}

void TFPlayerSystem::DestroyPawn(PlayerRec& rec)
{
    if (!rec.hasPawn)
        return;
    m_pawnOwner.erase(rec.pawn);
    rec.hasPawn = false;
    rec.alive = false;
    rec.pawn = 0;
    // TF-W1-FULL: destroy the ECS entity + unregister replication.
}

void TFPlayerSystem::SyncClientRecords() {}
void TFPlayerSystem::AttachPawnVisual(uint32_t, FactionId) {}

} // namespace Terrafront
