/**
 * @file TFReplicationClient.cpp
 * @brief TF replication channel (W1) — CLIENT side: handlers for the 0x54F0
 *        block maintaining the RemotePawn store (shared W1B contract) and the
 *        owner-only TF_MoveState feed, plus the debug panel. Lifecycle and
 *        the server broadcast side live in TFReplication.cpp.
 */
#include "Net/TFReplication.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <cstring>

namespace Terrafront {

// ---------------------------------------------------------------------------
// Shared W1B contract — client-side store
// ---------------------------------------------------------------------------

bool TFReplication::GetRemotePawn(EntityId entity, RemotePawn& out) const
{
    auto it = m_pawns.find(entity);
    if (it == m_pawns.end())
        return false;
    out = it->second;
    return true;
}

void TFReplication::ForEachRemotePawn(const std::function<void(const RemotePawn&)>& fn) const
{
    for (const auto& kv : m_pawns)
        fn(kv.second);
}

bool TFReplication::GetLatestMoveState(TF_MoveState& out) const
{
    if (!m_hasMoveState)
        return false;
    out = m_moveState;
    m_freshMoveState = false;
    return true;
}

#ifdef ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Handler registration
// ---------------------------------------------------------------------------

bool TFReplication::ClientActive() const
{
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    return nm.IsInitialized() &&
           nm.GetRole() == Spark::Net::NetworkRole::Client;
}

void TFReplication::ClientEnsureHandlers()
{
    using Spark::Net::MessageType;
    using Spark::Net::NetworkMessage;
    auto& nm = Spark::Net::NetworkManager::GetInstance();

    nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_Create),
                       [this](const NetworkMessage& m) { OnRepCreate(m.payload.data(), m.payload.size()); });
    nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_Update),
                       [this](const NetworkMessage& m) { OnRepUpdate(m.payload.data(), m.payload.size()); });
    nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_Destroy),
                       [this](const NetworkMessage& m) { OnRepDestroy(m.payload.data(), m.payload.size()); });
    nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_MoveState),
                       [this](const NetworkMessage& m) { OnRepMoveState(m.payload.data(), m.payload.size()); });

    m_clientHandlers = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] rep: client handlers registered");
}

void TFReplication::ClientReleaseHandlers()
{
    // NetworkManager has no per-type removal; replace with no-ops so no
    // dangling `this` survives module shutdown (same pattern as TFServerSim).
    using Spark::Net::MessageType;
    auto& nm = Spark::Net::NetworkManager::GetInstance();
    for (uint16_t id : {kTFRepMsg_Create, kTFRepMsg_Update, kTFRepMsg_Destroy,
                        kTFRepMsg_MoveState})
    {
        nm.RegisterHandler(static_cast<MessageType>(id),
                           [](const Spark::Net::NetworkMessage&) {});
    }
    m_clientHandlers = false;
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

void TFReplication::OnRepCreate(const void* data, size_t size)
{
    if (size != sizeof(TF_RepPawnCreate))
    {
        ++m_badPackets;
        return;
    }
    TF_RepPawnCreate c;
    std::memcpy(&c, data, sizeof(c));

    RemotePawn& p = m_pawns[c.entityId];
    p = RemotePawn{};
    p.entity   = c.entityId;
    p.owner    = c.ownerPlayer;
    p.faction  = static_cast<FactionId>(c.faction);
    p.cls      = static_cast<ClassId>(c.classId);
    p.alive    = c.alive != 0;
    p.pos[0] = c.posX; p.pos[1] = c.posY; p.pos[2] = c.posZ;
    p.yaw = c.yaw; p.pitch = c.pitch;
    p.health = c.health; p.shield = c.shield;
    p.recvTime = m_clock;

    ++m_ctrMsgs;
    m_ctrBytes += size;
}

void TFReplication::OnRepUpdate(const void* data, size_t size)
{
    if (size < sizeof(TF_RepUpdateHeader))
    {
        ++m_badPackets;
        return;
    }
    TF_RepUpdateHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));

    if (size != sizeof(hdr) + hdr.entityCount * sizeof(TF_RepPawnUpdate))
    {
        ++m_badPackets;
        return;
    }

    const uint8_t* cursor = static_cast<const uint8_t*>(data) + sizeof(hdr);
    for (uint16_t i = 0; i < hdr.entityCount; ++i, cursor += sizeof(TF_RepPawnUpdate))
    {
        TF_RepPawnUpdate rec;
        std::memcpy(&rec, cursor, sizeof(rec));

        auto it = m_pawns.find(rec.entityId);
        if (it == m_pawns.end())
        {
            // unreliable update raced ahead of the reliable Create — skip
            ++m_unknownEntityUpdates;
            continue;
        }
        RemotePawn& p = it->second;

        float newPos[3];
        rec.pos.To(newPos);

        // velocity estimate from successive updates (for interpolation /
        // extrapolation consumers; Create carries no velocity)
        const double dt = m_clock - p.recvTime;
        if (dt > 1.0e-4 && dt < 1.0)
        {
            const float inv = static_cast<float>(1.0 / dt);
            p.vel[0] = (newPos[0] - p.pos[0]) * inv;
            p.vel[1] = (newPos[1] - p.pos[1]) * inv;
            p.vel[2] = (newPos[2] - p.pos[2]) * inv;
        }

        p.pos[0] = newPos[0]; p.pos[1] = newPos[1]; p.pos[2] = newPos[2];
        p.yaw    = rec.aim.Yaw();
        p.pitch  = rec.aim.Pitch();
        p.health = static_cast<float>(rec.health);
        p.shield = static_cast<float>(rec.shield);
        p.alive  = rec.alive != 0;
        p.recvTime = m_clock;
    }

    ++m_ctrMsgs;
    m_ctrRecords += hdr.entityCount;
    m_ctrBytes += size;
}

void TFReplication::OnRepDestroy(const void* data, size_t size)
{
    if (size != sizeof(TF_RepDestroy))
    {
        ++m_badPackets;
        return;
    }
    TF_RepDestroy d;
    std::memcpy(&d, data, sizeof(d));
    m_pawns.erase(d.entityId);

    ++m_ctrMsgs;
    m_ctrBytes += size;
}

void TFReplication::OnRepMoveState(const void* data, size_t size)
{
    if (size != sizeof(TF_MoveState))
    {
        ++m_badPackets;
        return;
    }
    std::memcpy(&m_moveState, data, sizeof(m_moveState));
    m_hasMoveState = true;
    m_freshMoveState = true;

    ++m_ctrMsgs;
    m_ctrBytes += size;
}

#endif // ENABLE_NETWORKING

// ---------------------------------------------------------------------------
// Debug UI
// ---------------------------------------------------------------------------

void TFReplication::RenderDebugUI()
{
#ifdef SPARK_HAS_IMGUI
    if (!m_showDebug)
        return;
    if (ImGui::Begin("TF Replication", &m_showDebug))
    {
        const bool serverish = m_ctx && m_ctx->IsAuthority();
        ImGui::Text("mode         : %s", serverish ? "server (broadcast)" : "client (store)");
        ImGui::Text("rep msgs/sec : %.1f   records/sec: %.1f", m_msgsPerSec, m_recordsPerSec);
        ImGui::Text("bytes/sec    : %.0f (payloads, est.)", m_bytesPerSec);
        ImGui::Text("bad packets  : %u   unknown-entity updates: %u",
                    m_badPackets, m_unknownEntityUpdates);

        if (serverish)
        {
            ImGui::Separator();
            ImGui::Text("clients      : %zu", m_knownClients.size());
            ImGui::Text("pawns cached : %zu   unchanged skips: %u",
                        m_lastSent.size(), m_skippedUnchanged);
        }
        else
        {
            ImGui::Separator();
            ImGui::Text("remote pawns : %zu", m_pawns.size());
            ImGui::Text("move state   : %s%s", m_hasMoveState ? "yes" : "no",
                        m_freshMoveState ? " (fresh)" : "");
            for (const auto& kv : m_pawns)
            {
                const RemotePawn& p = kv.second;
                ImGui::Text("e%u p%u %s %s pos=(%.1f %.1f %.1f) hp=%.0f/%.0f age=%.2fs",
                            p.entity, p.owner, FactionTag(p.faction),
                            p.alive ? "alive" : "dead",
                            p.pos[0], p.pos[1], p.pos[2], p.health, p.shield,
                            m_clock - p.recvTime);
            }
        }
    }
    ImGui::End();
#endif // SPARK_HAS_IMGUI
}

} // namespace Terrafront
