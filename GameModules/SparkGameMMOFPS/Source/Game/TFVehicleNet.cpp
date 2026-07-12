/**
 * @file TFVehicleNet.cpp
 * @brief W3 vehicles — replication halves of TFVehicleSystem on the 0x54F8+
 *        block (TFRepProtocol.h): server create/update/destroy/seats
 *        broadcasting with dirty checks + late-join bursts, the C->S purchase
 *        handler, and the pure-client mirror store with interpolated OBJ
 *        visuals. W8 adds the turret-aim channel (0x5448, TFVehicleSystem.h):
 *        10 Hz-on-change / 1 Hz-keepalive S->C aim, mirror-side smoothing and
 *        the mirror aim-rig children. Server game logic lives in
 *        TFVehicleSystem.cpp, client UX in TFVehicleClient.cpp (same class,
 *        split per repo file-size rules).
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFVisualUtils.h"
#include "Net/TFServerSim.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;
        constexpr float kPi = 3.14159265f;

        uint16_t QuantHP(float v)
        {
            return static_cast<uint16_t>(std::lround(std::clamp(v, 0.0f, 65535.0f)));
        }

        int16_t QuantAngle(float rad)
        {
            return static_cast<int16_t>(std::lround(QuantAim::WrapPi(rad) * QuantAim::kScale));
        }

        // W8 turret aim quantizers: yaw [-pi, pi) -> uint16 full-turn; pitch to whole
        // degrees (int8 easily covers the clamped [-35, +10] deg range).
        uint16_t QuantYaw16(float rad)
        {
            const float w = QuantAim::WrapPi(rad); // [-pi, pi]
            const long q = std::lround((w + kPi) * (65535.0f / (2.0f * kPi)));
            return static_cast<uint16_t>(std::clamp(q, 0L, 65535L));
        }

        float DequantYaw16(uint16_t q)
        {
            return static_cast<float>(q) * (2.0f * kPi / 65535.0f) - kPi;
        }

        int8_t QuantPitchDeg(float rad)
        {
            return static_cast<int8_t>(std::lround(std::clamp(rad * kRadToDeg, -127.0f, 127.0f)));
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Fixed-tick driver (both roles; mirrors the TFReplication lazy-handler pattern)
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::NetFixedUpdate(float fdt)
    {
#ifdef ENABLE_NETWORKING
        // Server side: purchase handler + join bursts + 20 Hz dirty broadcast.
        if (ServerNetActive())
        {
            if (!m_serverHandlers)
                ServerEnsureNetHandlers();
            ServerPollNewClients();

            m_updateAccum += fdt;
            if (m_updateAccum >= 1.0f / kReplicationHz)
            {
                m_updateAccum = 0.0f;
                ServerBroadcastUpdates();
                ServerBroadcastAim(); // W8: own 10 Hz / 1 Hz throttle inside
            }
        }
        else
        {
            if (m_serverHandlers)
                ServerReleaseNetHandlers();
            if (!m_knownClients.empty() || !m_lastSent.empty() || !m_lastAimSent.empty())
            {
                m_knownClients.clear();
                m_lastSent.clear();
                m_lastAimSent.clear();
            }
        }

        // Client side: mirror handlers while a client link is up.
        if (ClientNetActive())
        {
            if (!m_clientHandlers)
                ClientEnsureHandlers();
        }
        else if (m_clientHandlers)
        {
            ClientReleaseHandlers();
            ClientDropMirror();
        }
#else
        (void)fdt;
#endif
    }

#ifdef ENABLE_NETWORKING

    bool TFVehicleSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
    }

    bool TFVehicleSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Client;
    }

    // ---------------------------------------------------------------------------
    // Server: handlers + broadcast
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ServerEnsureNetHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        // C->S purchase requests land here; seat/deploy ops route through
        // TFServerSim (frozen TFMsg ids) into ServerHandleSeatOp/AegisDeploy.
        nm.RegisterHandler(static_cast<MessageType>(kTFVehMsg_Purchase), [this](const NetworkMessage& m)
                           { OnNetPurchase(m.senderID, m.payload.data(), m.payload.size()); });
        m_serverHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] vehicle server net handlers registered");
    }

    void TFVehicleSystem::ServerReleaseNetHandlers()
    {
        // No per-type removal in NetworkManager; overwrite with a no-op so no
        // dangling `this` survives shutdown (module-wide pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFVehMsg_Purchase),
                           [](const Spark::Net::NetworkMessage&) {});
        m_serverHandlers = false;
    }

    void TFVehicleSystem::OnNetPurchase(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_VehPurchase) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_VehPurchase req;
        std::memcpy(&req, data, sizeof(req));
        if (req.vehId >= static_cast<uint8_t>(VehicleId::COUNT))
        {
            ++m_badPackets;
            return;
        }
        ServerPurchaseVehicle(sender, static_cast<VehicleId>(req.vehId));
    }

    void TFVehicleSystem::ServerPollNewClients()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const auto& clients = nm.GetClients();

        for (const auto& [id, info] : clients)
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            const PlayerId joined = id;
            m_knownClients.insert(joined);
            // Late joiner: full vehicle set as reliable creates + seat tables
            // (+ current turret aim so rigs don't sit hull-forward for up to 1 s).
            for (const VehicleRec& v : m_vehicles)
            {
                ServerSendCreate(joined, v);
                ServerSendSeats(joined, v);
                if (TurretControllerSeat(DefOf(v.vehId)) >= 0)
                    ServerSendAim(joined, v, true);
            }
        }
        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            if (!clients.contains(*it))
                it = m_knownClients.erase(it);
            else
                ++it;
        }
    }

    void TFVehicleSystem::ServerBroadcastUpdates()
    {
        std::vector<TF_RepVehicleUpdate> records;
        for (VehicleRec& v : m_vehicles)
        {
            // Reliable seat/deploy deltas ride their own message.
            if (v.seatsDirty)
            {
                v.seatsDirty = false;
                ServerSendSeats(kInvalidPlayer, v);
            }

            SentState now;
            now.pos = QuantPos::From(v.pos);
            now.yaw10k = QuantAngle(v.yaw);
            now.pitch10k = QuantAngle(v.pitch);
            now.roll10k = QuantAngle(v.roll);
            now.health = QuantHP(v.hp);
            now.deployed = v.deployed ? 1 : 0;

            auto it = m_lastSent.find(v.entity);
            if (it != m_lastSent.end() && it->second == now)
                continue; // unchanged — zero bandwidth (pawn-channel rule)
            m_lastSent[v.entity] = now;

            TF_RepVehicleUpdate rec{};
            rec.entityId = v.entity;
            rec.pos = now.pos;
            rec.yaw10k = now.yaw10k;
            rec.pitch10k = now.pitch10k;
            rec.roll10k = now.roll10k;
            rec.health = now.health;
            rec.deployed = now.deployed;
            records.push_back(rec);
        }
        if (records.empty())
            return;

        TF_RepUpdateHeader hdr{};
        hdr.serverTime = m_ctx->serverSim ? static_cast<float>(m_ctx->serverSim->ServerTime()) : 0.0f;
        hdr.entityCount = static_cast<uint16_t>(records.size());

        std::vector<uint8_t> payload(sizeof(hdr) + records.size() * sizeof(TF_RepVehicleUpdate));
        std::memcpy(payload.data(), &hdr, sizeof(hdr));
        std::memcpy(payload.data() + sizeof(hdr), records.data(), records.size() * sizeof(TF_RepVehicleUpdate));
        SendNet(kInvalidPlayer, kTFRepMsg_VehUpdate, payload.data(), payload.size(), false);
    }

    // ---------------------------------------------------------------------------
    // W8 turret aim broadcast (0x5448): 10 Hz while the aim moves, 1 Hz keepalive.
    // Runs from ServerBroadcastUpdates' 20 Hz gate; the throttle below decides.
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ServerBroadcastAim()
    {
        const double now = m_ctx->serverSim ? m_ctx->serverSim->ServerTime() : m_clock;
        constexpr double kActivePeriodSec = 0.1; // 10 Hz while changing
        constexpr double kIdlePeriodSec = 1.0;   // 1 Hz keepalive

        for (const VehicleRec& v : m_vehicles)
        {
            if (TurretControllerSeat(DefOf(v.vehId)) < 0)
                continue; // no armed seat -> nothing aims

            const uint16_t yaw16 = QuantYaw16(v.aimYaw);
            const int8_t pitchDeg = QuantPitchDeg(v.aimPitch);
            AimSent& sent = m_lastAimSent[v.entity];
            const bool changed = yaw16 != sent.yaw16 || pitchDeg != sent.pitchDeg;
            const double since = now - sent.lastSend;
            if (!(changed && since >= kActivePeriodSec) && since < kIdlePeriodSec)
                continue;
            sent.yaw16 = yaw16;
            sent.pitchDeg = pitchDeg;
            sent.lastSend = now;
            ServerSendAim(kInvalidPlayer, v, false);
        }
    }

    void TFVehicleSystem::ServerSendAim(PlayerId target, const VehicleRec& v, bool reliable)
    {
        TF_RepVehicleAim a{};
        a.entityId = v.entity;
        a.yaw16 = QuantYaw16(v.aimYaw);
        a.pitchDeg = QuantPitchDeg(v.aimPitch);
        SendNet(target, kTFVehMsg_TurretAim, &a, sizeof(a), reliable);
    }

    void TFVehicleSystem::ServerSendCreate(PlayerId target, const VehicleRec& v)
    {
        TF_RepVehicleCreate c{};
        c.entityId = v.entity;
        c.vehId = static_cast<uint8_t>(v.vehId);
        c.faction = static_cast<uint8_t>(v.faction);
        c.deployed = v.deployed ? 1 : 0;
        c.posX = v.pos[0];
        c.posY = v.pos[1];
        c.posZ = v.pos[2];
        c.yaw = v.yaw;
        c.health = v.hp;
        c.maxHealth = v.maxHp;
        SendNet(target, kTFRepMsg_VehCreate, &c, sizeof(c), true);
    }

    void TFVehicleSystem::ServerSendSeats(PlayerId target, const VehicleRec& v)
    {
        TF_RepVehicleSeats s{};
        s.entityId = v.entity;
        for (int i = 0; i < 8; ++i)
            s.seats[i] = v.seats[i];
        s.seatCount = v.seatCount;
        s.deployed = v.deployed ? 1 : 0;
        SendNet(target, kTFRepMsg_VehSeats, &s, sizeof(s), true);
    }

    void TFVehicleSystem::ServerSendDestroy(EntityId vehicle)
    {
        TF_RepDestroy d{vehicle};
        SendNet(kInvalidPlayer, kTFRepMsg_VehDestroy, &d, sizeof(d), true);
    }

    void TFVehicleSystem::SendNet(PlayerId target, uint16_t msgId, const void* payload, size_t size, bool reliable)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = reliable ? Spark::Net::ChannelType::Reliable : Spark::Net::ChannelType::Unreliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        if (target == kInvalidPlayer)
            nm.SendToAll(msg);
        else
            nm.SendToClient(target, msg);
    }

    // ---------------------------------------------------------------------------
    // Client: mirror handlers
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ClientEnsureHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_VehCreate),
                           [this](const NetworkMessage& m) { OnNetVehCreate(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_VehUpdate),
                           [this](const NetworkMessage& m) { OnNetVehUpdate(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_VehDestroy),
                           [this](const NetworkMessage& m) { OnNetVehDestroy(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_VehSeats),
                           [this](const NetworkMessage& m) { OnNetVehSeats(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFVehMsg_TurretAim),
                           [this](const NetworkMessage& m) { OnNetVehAim(m.payload.data(), m.payload.size()); });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] vehicle client handlers registered");
    }

    void TFVehicleSystem::ClientReleaseHandlers()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id :
             {kTFRepMsg_VehCreate, kTFRepMsg_VehUpdate, kTFRepMsg_VehDestroy, kTFRepMsg_VehSeats, kTFVehMsg_TurretAim})
        {
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        }
        m_clientHandlers = false;
    }

    void TFVehicleSystem::OnNetVehCreate(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepVehicleCreate))
        {
            ++m_badPackets;
            return;
        }
        TF_RepVehicleCreate c;
        std::memcpy(&c, data, sizeof(c));

        MirrorRec& m = m_mirror[c.entityId];
        const uint32_t keepLocal = m.local; // re-create for the same id keeps the visual
        const TurretRig keepRig = m.rig;    // ... and its aim-rig children
        m = MirrorRec{};
        m.local = keepLocal;
        m.rig = keepRig;
        m.info.entity = c.entityId;
        m.info.vehId = static_cast<VehicleId>(c.vehId);
        m.info.faction = static_cast<FactionId>(c.faction);
        m.info.deployed = c.deployed != 0;
        m.info.pos[0] = c.posX;
        m.info.pos[1] = c.posY;
        m.info.pos[2] = c.posZ;
        m.info.yaw = c.yaw;
        m.info.hp = c.health;
        m.info.maxHp = c.maxHealth;
        if (const VehicleDef* def = DefOf(m.info.vehId))
            m.info.seatCount = static_cast<uint8_t>(std::min<size_t>(def->seats.size(), 8));
        m.targetPos[0] = c.posX;
        m.targetPos[1] = c.posY;
        m.targetPos[2] = c.posZ;
        m.targetYaw = c.yaw;
        m.hasTarget = true;
        m.aimYaw = m.targetAimYaw = c.yaw; // turret starts hull-forward, level
        MirrorAttachVisual(m);
    }

    void TFVehicleSystem::OnNetVehUpdate(const void* data, size_t size)
    {
        if (size < sizeof(TF_RepUpdateHeader))
        {
            ++m_badPackets;
            return;
        }
        TF_RepUpdateHeader hdr;
        std::memcpy(&hdr, data, sizeof(hdr));
        if (size != sizeof(hdr) + hdr.entityCount * sizeof(TF_RepVehicleUpdate))
        {
            ++m_badPackets;
            return;
        }

        const uint8_t* cursor = static_cast<const uint8_t*>(data) + sizeof(hdr);
        for (uint16_t i = 0; i < hdr.entityCount; ++i, cursor += sizeof(TF_RepVehicleUpdate))
        {
            TF_RepVehicleUpdate rec;
            std::memcpy(&rec, cursor, sizeof(rec));
            auto it = m_mirror.find(rec.entityId);
            if (it == m_mirror.end())
                continue; // unreliable update raced the reliable create
            MirrorRec& m = it->second;
            rec.pos.To(m.targetPos);
            m.targetYaw = rec.yaw10k / QuantAim::kScale;
            m.targetPitch = rec.pitch10k / QuantAim::kScale;
            m.targetRoll = rec.roll10k / QuantAim::kScale;
            m.hasTarget = true;
            m.info.hp = static_cast<float>(rec.health);
            m.info.deployed = rec.deployed != 0;
        }
    }

    void TFVehicleSystem::OnNetVehDestroy(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepDestroy))
        {
            ++m_badPackets;
            return;
        }
        TF_RepDestroy d;
        std::memcpy(&d, data, sizeof(d));

        auto it = m_mirror.find(d.entityId);
        if (it != m_mirror.end())
        {
            if (const VehicleDef* def = DefOf(it->second.info.vehId))
                PlayOneShot(def->explodeAudio);
            DestroyTurretRig(it->second.rig); // rig children before the hull
            // W13: leave a charred, static wreck (see TFVehicleSystem.cpp
            // SpawnWreck/UpdateWrecks) instead of an immediate despawn. Erased
            // from m_mirror below, so this hull stops being a "vehicle" to
            // every mirror-side query the instant it becomes a wreck.
            SpawnWreck(it->second.local);
            m_mirror.erase(it);
        }
        m_mirrorSeats.erase(d.entityId);
    }

    void TFVehicleSystem::OnNetVehSeats(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepVehicleSeats))
        {
            ++m_badPackets;
            return;
        }
        TF_RepVehicleSeats s;
        std::memcpy(&s, data, sizeof(s));
        m_mirrorSeats[s.entityId] = s;

        auto it = m_mirror.find(s.entityId);
        if (it != m_mirror.end())
        {
            MirrorRec& m = it->second;
            m.info.deployed = s.deployed != 0;
            m.info.seatCount = std::min<uint8_t>(s.seatCount, 8);
            for (int i = 0; i < 8; ++i)
                m.info.seats[i] = s.seats[i];
        }
    }

    void TFVehicleSystem::OnNetVehAim(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepVehicleAim))
        {
            ++m_badPackets;
            return;
        }
        TF_RepVehicleAim a;
        std::memcpy(&a, data, sizeof(a));

        auto it = m_mirror.find(a.entityId);
        if (it == m_mirror.end())
            return; // aim raced the reliable create — keepalive refreshes it
        it->second.targetAimYaw = DequantYaw16(a.yaw16);
        it->second.targetAimPitch = static_cast<float>(a.pitchDeg) / kRadToDeg;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Client mirror visuals (compiled on all configs; no-ops without a mirror)
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::MirrorAttachVisual(MirrorRec& m)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || m.local != 0)
            return;
        const VehicleDef* def = DefOf(m.info.vehId);
        if (!def)
            return;

        EntityID e = world->CreateEntity("TF_VehMirror_" + def->name);
        if (static_cast<uint32_t>(e) == 0)
            e = world->CreateEntity("TF_VehMirror_" + def->name);

        Transform& t = world->AddComponent<Transform>(e);
        t.position = {m.info.pos[0], m.info.pos[1], m.info.pos[2]};
        t.rotation.y = m.info.yaw * kRadToDeg;

        if (!def->model.empty())
        {
            MeshRenderer& mr = world->AddComponent<MeshRenderer>(e);
            mr.meshPath = "Assets/" + def->model;
            mr.materialPath = FactionStructureMaterial(*m_ctx, m.info.faction);
            mr.castShadows = true;
        }
        m.local = static_cast<uint32_t>(e);

        // W8: mirrors get the same turret/barrel/head rig as server visuals (before
        // this the turret child only existed on authority roles at all).
        AttachTurretRig(m.local, *def, m.info.faction, m.rig);
    }

    void TFVehicleSystem::ClientInterpolate(float dt)
    {
        if (m_mirror.empty())
            return;
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;

        // Exponential smoothing toward the newest replicated pose — vehicles are
        // big and slow relative to pawns, so this reads as clean as the pawn
        // channel's delay buffers at a fraction of the bookkeeping.
        const float k = std::min(1.0f, 10.0f * dt);
        for (auto& [entity, m] : m_mirror)
        {
            if (!m.hasTarget)
                continue;
            for (int i = 0; i < 3; ++i)
                m.info.pos[i] += (m.targetPos[i] - m.info.pos[i]) * k;

            // shortest-arc yaw approach
            float dyaw = QuantAim::WrapPi(m.targetYaw - m.info.yaw);
            m.info.yaw = QuantAim::WrapPi(m.info.yaw + dyaw * k);

            // W8 turret aim: slightly faster approach than the hull so the turret
            // reads responsive between 10 Hz aim packets.
            const float ka = std::min(1.0f, 14.0f * dt);
            const float dAimYaw = QuantAim::WrapPi(m.targetAimYaw - m.aimYaw);
            m.aimYaw = QuantAim::WrapPi(m.aimYaw + dAimYaw * ka);
            m.aimPitch += (m.targetAimPitch - m.aimPitch) * ka;

            if (world && m.local != 0)
            {
                const auto e = static_cast<EntityID>(m.local);
                if (world->GetRegistry().valid(e))
                {
                    if (Transform* t = world->GetComponent<Transform>(e))
                    {
                        t->position = {m.info.pos[0], m.info.pos[1], m.info.pos[2]};
                        t->rotation = {m.targetPitch * kRadToDeg, m.info.yaw * kRadToDeg, m.targetRoll * kRadToDeg};
                    }
                }
                ApplyTurretPose(m.rig, m.info.yaw, m.aimYaw, m.aimPitch);
            }
        }
    }

    void TFVehicleSystem::ClientDropMirror()
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        for (auto& [entity, m] : m_mirror)
        {
            DestroyTurretRig(m.rig); // rig children before the hull
            const auto e = static_cast<EntityID>(m.local);
            if (world && m.local != 0 && world->GetRegistry().valid(e))
                world->DestroyEntity(e);
        }
        m_mirror.clear();
        m_mirrorSeats.clear();
    }

} // namespace Terrafront
