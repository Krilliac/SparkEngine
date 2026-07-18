/**
 * @file TFVehicleNetMirror.cpp
 * @brief W3 vehicles — pure-client mirror half of TFVehicleSystem replication
 *        on the 0x54F8+ block (TFRepProtocol.h): the create/update/destroy/
 *        seats handlers feeding the mirror store, W8 turret-aim reception
 *        (0x5448, TFVehicleSystem.h) with mirror-side smoothing, and the
 *        interpolated OBJ visuals with the mirror aim-rig children. Split
 *        from TFVehicleNet.cpp (fixed-tick driver + server broadcast half);
 *        shared internals live in TFVehicleSystemInternal.h.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFVehicleSystemInternal.h"
#include "Game/TFVisualUtils.h"
#include "Net/TFRepProtocol.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Terrafront
{

    using namespace VehicleDetail;

    namespace
    {

        float DequantYaw16(uint16_t q)
        {
            return static_cast<float>(q) * (2.0f * kPi / 65535.0f) - kPi;
        }

    } // namespace

#ifdef ENABLE_NETWORKING

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
