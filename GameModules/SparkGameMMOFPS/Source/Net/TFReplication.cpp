/**
 * @file TFReplication.cpp
 * @brief TF replication channel (W1) — lifecycle + SERVER side: pawn
 *        create/update/destroy broadcasting on the 0x54F0 block. The client
 *        side (RemotePawn store + handlers) lives in TFReplicationClient.cpp.
 *
 * Design note: the engine's EntityReplicator/ReplicatedEntity system exists,
 * but TFServerSim already established this channel as compact packed structs
 * (kTFRepMsg_MoveState). W1 stays consistent with that wire protocol instead
 * of running a second, string-keyed replication path in parallel.
 */
#include "Net/TFReplication.h"

#include "Game/TFPlayerSystem.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Net/TFServerSim.h"   // ServerTime() for TF_RepUpdateHeader.serverTime
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Terrafront {

namespace {

constexpr float kRateWindowSec = 1.0f;   // stats sampling window

uint16_t QuantHP(float v)
{
    return static_cast<uint16_t>(std::lround(std::clamp(v, 0.0f, 65535.0f)));
}

} // namespace

TFReplication::TFReplication() = default;
TFReplication::~TFReplication() { if (m_initialized) Shutdown(); }

bool TFReplication::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });
    events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFReplication initialized");
    return true;
}

void TFReplication::Update(float deltaTime)
{
    if (!m_initialized)
        return;

    m_clock += deltaTime;

    m_rateWindow += deltaTime;
    if (m_rateWindow >= kRateWindowSec)
    {
        m_msgsPerSec    = static_cast<float>(m_ctrMsgs) / m_rateWindow;
        m_recordsPerSec = static_cast<float>(m_ctrRecords) / m_rateWindow;
        m_bytesPerSec   = static_cast<float>(m_ctrBytes) / m_rateWindow;
        m_ctrMsgs = 0;
        m_ctrRecords = 0;
        m_ctrBytes = 0;
        m_rateWindow = 0.0f;
    }
}

void TFReplication::FixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || !m_ctx)
        return;

#ifdef ENABLE_NETWORKING
    // --- server side: welcome late joiners + 20 Hz state broadcast ---------
    if (ServerActive())
    {
        ServerPollNewClients();

        m_updateAccum += fixedDeltaTime;
        if (m_updateAccum >= 1.0f / kReplicationHz)
        {
            m_updateAccum = 0.0f;
            ServerBroadcastUpdates();
        }
    }
    else if (!m_knownClients.empty() || !m_lastSent.empty())
    {
        // server went down — drop broadcast bookkeeping
        m_knownClients.clear();
        m_lastSent.clear();
    }

    // --- client side: lazy handler registration mirroring TFServerSim ------
    if (ClientActive())
    {
        if (!m_clientHandlers)
            ClientEnsureHandlers();
    }
    else if (m_clientHandlers)
    {
        ClientReleaseHandlers();
        m_pawns.clear();
        m_hasMoveState = false;
        m_freshMoveState = false;
    }
#else
    (void)fixedDeltaTime;
#endif
}

void TFReplication::Shutdown()
{
    if (!m_initialized)
        return;
#ifdef ENABLE_NETWORKING
    if (m_clientHandlers)
        ClientReleaseHandlers();
#endif
    m_knownClients.clear();
    m_lastSent.clear();
    m_pawns.clear();
    m_hasMoveState = false;
    m_freshMoveState = false;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Event handlers (server broadcast triggers)
// ---------------------------------------------------------------------------

TFReplication::SentState TFReplication::QuantizePawn(const PawnInfo& p)
{
    SentState s;
    s.pos    = QuantPos::From(p.pos);
    s.aim    = QuantAim::From(p.yaw, p.pitch);
    s.health = QuantHP(p.health);
    s.shield = QuantHP(p.shield);
    s.alive  = p.alive ? 1 : 0;
    return s;
}

void TFReplication::OnPlayerSpawned(const EvPlayerSpawned& ev)
{
#ifdef ENABLE_NETWORKING
    if (!ServerActive() || !m_ctx->players)
        return;
    PawnInfo p{};
    if (!m_ctx->players->GetPawnByEntity(ev.pawn, p))
    {
        // registry not filled yet — replicate identity from the event alone
        p.entity  = ev.pawn;
        p.owner   = ev.player;
        p.faction = ev.faction;
        p.cls     = ev.cls;
        p.alive   = true;
    }
    const TF_RepPawnCreate c = MakeCreate(p);
    SendRep(kInvalidPlayer, kTFRepMsg_Create, &c, sizeof(c), true);
    m_lastSent[p.entity] = QuantizePawn(p);
#else
    (void)ev;
#endif
}

void TFReplication::OnPlayerKilled(const EvPlayerKilled& ev)
{
#ifdef ENABLE_NETWORKING
    if (!ServerActive() || !m_ctx->players)
        return;
    PawnInfo p{};
    if (m_ctx->players->GetPawnByPlayer(ev.victim, p) && p.entity != 0)
        ServerBroadcastDestroy(p.entity);
#else
    (void)ev;
#endif
}

#ifdef ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Server side
// ---------------------------------------------------------------------------

bool TFReplication::ServerActive() const
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    return nm.IsInitialized() &&
           nm.GetRole() == Spark::Net::NetworkRole::Server &&
           m_ctx->IsAuthority();
}

TF_RepPawnCreate TFReplication::MakeCreate(const PawnInfo& p)
{
    TF_RepPawnCreate c{};
    c.entityId    = p.entity;
    c.ownerPlayer = p.owner;
    c.faction     = static_cast<uint8_t>(p.faction);
    c.classId     = static_cast<uint8_t>(p.cls);
    c.alive       = p.alive ? 1 : 0;
    c.posX = p.pos[0]; c.posY = p.pos[1]; c.posZ = p.pos[2];
    c.yaw = p.yaw; c.pitch = p.pitch;
    c.health = p.health; c.shield = p.shield;
    return c;
}

void TFReplication::ServerPollNewClients()
{
    // NetworkManager exposes no join callback registry (SetTimeoutHandler is a
    // single slot owned elsewhere), so mirror TFServerSim: diff GetClients().
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    const auto& clients = nm.GetClients();

    for (const auto& [id, info] : clients)
    {
        if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
            continue;
        const PlayerId joined = id; // plain local (structured bindings + lambda capture)
        m_knownClients.insert(joined);

        // Late joiner: send the full current pawn set as reliable Creates.
        uint32_t sent = 0;
        if (m_ctx->players)
        {
            m_ctx->players->ForEachAlivePawn([this, joined, &sent](const PawnInfo& p) {
                const TF_RepPawnCreate c = MakeCreate(p);
                SendRep(joined, kTFRepMsg_Create, &c, sizeof(c), true);
                ++sent;
            });
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] rep: client %u joined — %u pawn creates sent", joined, sent);
    }

    for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
    {
        if (!clients.contains(*it))
            it = m_knownClients.erase(it);
        else
            ++it;
    }
}

void TFReplication::ServerBroadcastUpdates()
{
    if (!m_ctx->players)
        return;

    std::vector<TF_RepPawnUpdate> records;
    std::unordered_set<EntityId> alive;

    m_ctx->players->ForEachAlivePawn([this, &records, &alive](const PawnInfo& p) {
        alive.insert(p.entity);

        const SentState now = QuantizePawn(p);
        auto it = m_lastSent.find(p.entity);
        if (it != m_lastSent.end() && it->second == now)
        {
            ++m_skippedUnchanged;
            return; // quantized state unchanged — costs zero bandwidth
        }
        m_lastSent[p.entity] = now;

        TF_RepPawnUpdate rec{};
        rec.entityId = p.entity;
        rec.pos      = now.pos;
        rec.aim      = now.aim;
        rec.health   = now.health;
        rec.shield   = now.shield;
        rec.alive    = now.alive;
        records.push_back(rec);
    });

    // Sweep: entities we replicated before that silently left the alive set
    // (despawn without a kill event, e.g. cleanup paths) get a Destroy.
    std::vector<EntityId> stale;
    for (const auto& kv : m_lastSent)
        if (!alive.contains(kv.first))
            stale.push_back(kv.first);
    for (EntityId entity : stale)
        ServerBroadcastDestroy(entity);

    if (records.empty())
        return;

    TF_RepUpdateHeader hdr{};
    hdr.serverTime  = m_ctx->serverSim
                        ? static_cast<float>(m_ctx->serverSim->ServerTime()) : 0.0f;
    hdr.entityCount = static_cast<uint16_t>(records.size());

    std::vector<uint8_t> payload(sizeof(hdr) + records.size() * sizeof(TF_RepPawnUpdate));
    std::memcpy(payload.data(), &hdr, sizeof(hdr));
    std::memcpy(payload.data() + sizeof(hdr), records.data(),
                records.size() * sizeof(TF_RepPawnUpdate));

    SendRep(kInvalidPlayer, kTFRepMsg_Update, payload.data(), payload.size(), false);
    m_ctrRecords += static_cast<uint32_t>(records.size());
}

void TFReplication::ServerBroadcastDestroy(EntityId entity)
{
    TF_RepDestroy d{entity};
    SendRep(kInvalidPlayer, kTFRepMsg_Destroy, &d, sizeof(d), true);
    m_lastSent.erase(entity);
}

void TFReplication::SendRep(PlayerId target, uint16_t msgId, const void* payload,
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
    ++m_ctrMsgs;
    m_ctrBytes += size;
}

#endif // ENABLE_NETWORKING

} // namespace Terrafront
