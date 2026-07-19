/**
 * @file TFGrenadeSystemServer.cpp
 * @brief TFGrenadeSystem server half: throw validation + spawn, ballistic
 *        simulation with world-static/terrain bounces, detonation (splash
 *        damage / smoke / flash), and the S->C broadcast/unicast machinery.
 *        Split from TFGrenadeSystem.cpp; the shared view-direction helper
 *        lives in TFGrenadeSystemInternal.h.
 */
#include "Game/TFGrenadeSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFBallistics.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFGrenadeSystemInternal.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFWeaponMath.h"
#include "World/TFWorldSetup.h"

#include "Physics/PhysicsSystem.h" // engine umbrella header; stub-safe when Jolt is absent
#include "Spark/IEngineContext.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Terrafront
{

    using namespace GrenadeSysDetail;

    namespace
    {
        constexpr uint8_t kDamageKindExplosive = 1; // TF_DamageEvent damageKind convention

        // loadout-depth wave: smoke tuning.
        constexpr float kTFSmokeLifeSec = 6.0f; ///< client-side puff presentation lifetime
    } // namespace

    // ---------------------------------------------------------------------------
    // Server: throw validation + spawn
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ServerHandleThrowMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        if (size != sizeof(TF_GrenadeThrow) || !data)
        {
            ++m_badPackets;
            return;
        }
        TF_GrenadeThrow msg{};
        std::memcpy(&msg, data, sizeof(msg));
        if (!std::isfinite(msg.viewYaw) || !std::isfinite(msg.viewPitch))
        {
            ++m_badPackets;
            return;
        }
        float dir[3];
        ViewDir(msg.viewYaw, std::clamp(msg.viewPitch, -1.5707963f, 1.5707963f), dir);
        ServerTryThrow(sender, dir);
    }

    bool TFGrenadeSystem::CanThrowGrenade(PlayerId player) const
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->players || player == kInvalidPlayer)
            return false;
        if (m_live.size() >= kTFMaxLiveGrenades || !SelectedGrenadeDef(player))
            return false;
        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
            return false;
        const auto it = m_throwers.find(player);
        if (it == m_throwers.end())
        {
            // No record yet: lazily seeded on the first throw; class quota decides.
            const ClassDef* cls = m_ctx->data ? m_ctx->data->GetClass(pawn.cls) : nullptr;
            return cls && cls->grenades > 0;
        }
        return it->second.remaining > 0 && NowSec() >= it->second.nextThrowAt;
    }

    bool TFGrenadeSystem::ServerBotThrowGrenade(PlayerId player, const float dirUnit[3])
    {
        if (!dirUnit)
            return false;
        float dir[3] = {dirUnit[0], dirUnit[1], dirUnit[2]};
        if (!WeaponMath::Normalize3(dir))
            return false;
        return ServerTryThrow(player, dir);
    }

    int TFGrenadeSystem::GrenadesRemaining(PlayerId player) const
    {
        const auto it = m_throwers.find(player);
        return it == m_throwers.end() ? -1 : it->second.remaining;
    }

    bool TFGrenadeSystem::ServerTryThrow(PlayerId player, const float dirUnit[3])
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->players || player == kInvalidPlayer)
            return false;

        const WeaponDef* def = SelectedGrenadeDef(player); // loadout-depth wave: frag/smoke/flash
        if (!def || m_live.size() >= kTFMaxLiveGrenades)
        {
            ++m_rejected;
            return false;
        }

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
        {
            ++m_rejected;
            return false;
        }

        // Per-life quota record — normally seeded by EvPlayerSpawned; lazily
        // seeded here for pawns that predate this system's Initialize.
        auto [it, inserted] = m_throwers.try_emplace(player);
        ThrowerRec& rec = it->second;
        if (inserted)
        {
            const ClassDef* cls = m_ctx->data->GetClass(pawn.cls);
            rec.remaining = cls ? cls->grenades : 0;
        }

        const double now = NowSec();
        if (rec.remaining <= 0 || now < rec.nextThrowAt)
        {
            ++m_rejected;
            return false;
        }

        ServerGrenade g;
        g.id = m_nextGrenadeId++;
        if (m_nextGrenadeId == 0)
            m_nextGrenadeId = 1;
        g.thrower = player;
        g.throwerPawn = pawn.entity;
        g.weapon = def->id;
        g.splashRadiusM = def->splashRadiusM;
        g.splashDamage = def->splashDamage;
        g.vsVehicleMult = def->vsVehicleMult;
        g.gravityFactor = def->gravity > 0.0f ? def->gravity : 1.0f;
        const float speed = def->projSpeed > 0.0f ? def->projSpeed : kTFGrenadeFallbackThrowMps;
        // Eye origin + a small forward nudge (grenades ignore pawn capsules, so
        // this only keeps the first visual frame out of the camera).
        g.pos[0] = pawn.pos[0] + dirUnit[0] * 0.3f;
        g.pos[1] = pawn.pos[1] + WeaponMath::kEyeHeightM + dirUnit[1] * 0.3f;
        g.pos[2] = pawn.pos[2] + dirUnit[2] * 0.3f;
        g.vel[0] = dirUnit[0] * speed;
        g.vel[1] = dirUnit[1] * speed;
        g.vel[2] = dirUnit[2] * speed;
        g.effect = EffectKindOf(def->key); // loadout-depth wave

        rec.remaining -= 1;
        rec.nextThrowAt = now + kTFGrenadeThrowIntervalSec;
        ++m_throws;
        m_live.push_back(g);

        TF_GrenadeSpawn sp{};
        sp.grenadeId = g.id;
        sp.posQX = GrenadeDetail::QuantPos(g.pos[0]);
        sp.posQY = GrenadeDetail::QuantPos(g.pos[1]);
        sp.posQZ = GrenadeDetail::QuantPos(g.pos[2]);
        sp.velQX = GrenadeDetail::QuantVel(g.vel[0]);
        sp.velQY = GrenadeDetail::QuantVel(g.vel[1]);
        sp.velQZ = GrenadeDetail::QuantVel(g.vel[2]);
        sp.remaining = static_cast<uint8_t>(std::clamp(rec.remaining, 0, 255));
        sp.player = player;
        ServerBroadcast(kTFMsgGrenadeSpawn, &sp, sizeof(sp), /*reliable*/ true);
        return true;
    }

    // ---------------------------------------------------------------------------
    // Server: simulation
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ServerStepGrenade(ServerGrenade& g, float fdt)
    {
        const float prev[3] = {g.pos[0], g.pos[1], g.pos[2]};
        Ballistics::IntegrateProjectile(g.pos, g.vel, g.gravityFactor, fdt);

        // World statics / deployables: one filtered segment raycast per step
        // (<= kTFMaxLiveGrenades rays per tick). Pawn/vehicle layers are
        // deliberately excluded — grenades pass through actors.
        float seg[3] = {g.pos[0] - prev[0], g.pos[1] - prev[1], g.pos[2] - prev[2]};
        const float segLen = WeaponMath::Len3(seg);
        if (segLen > 1.0e-4f && m_ctx->engine)
        {
            if (::PhysicsSystem* physics = m_ctx->engine->GetPhysics())
            {
                const DirectX::XMFLOAT3 o{prev[0], prev[1], prev[2]};
                const DirectX::XMFLOAT3 d{seg[0] / segLen, seg[1] / segLen, seg[2] / segLen};
                const RaycastHit hit =
                    physics->RaycastFiltered(o, d, segLen, CollisionLayers::WorldStatic | CollisionLayers::Deployable);
                if (hit.hasHit && hit.distance <= segLen)
                {
                    float n[3] = {hit.normal.x, hit.normal.y, hit.normal.z};
                    if (!WeaponMath::Normalize3(n))
                    {
                        n[0] = 0.0f;
                        n[1] = 1.0f;
                        n[2] = 0.0f;
                    }
                    g.pos[0] = hit.point.x + n[0] * 0.03f;
                    g.pos[1] = hit.point.y + n[1] * 0.03f;
                    g.pos[2] = hit.point.z + n[2] * 0.03f;
                    ServerBounce(g, n);
                }
            }
        }

        // Terrain heightfield: reflect about the finite-difference normal.
        if (m_ctx->world)
        {
            const float h = m_ctx->world->TerrainHeightAt(g.pos[0], g.pos[2]);
            if (g.pos[1] < h + kTFGrenadeRadiusM)
            {
                constexpr float kStepM = 0.5f;
                const float hX = m_ctx->world->TerrainHeightAt(g.pos[0] + kStepM, g.pos[2]);
                const float hZ = m_ctx->world->TerrainHeightAt(g.pos[0], g.pos[2] + kStepM);
                float n[3] = {(h - hX) / kStepM, 1.0f, (h - hZ) / kStepM};
                if (!WeaponMath::Normalize3(n))
                {
                    n[0] = 0.0f;
                    n[1] = 1.0f;
                    n[2] = 0.0f;
                }
                g.pos[1] = h + kTFGrenadeRadiusM;
                ServerBounce(g, n);
            }
        }
    }

    void TFGrenadeSystem::ServerBounce(ServerGrenade& g, const float normal[3])
    {
        const float vn = WeaponMath::Dot3(g.vel, normal);
        if (vn < 0.0f)
        {
            // Split into normal/tangential parts: reflect the normal part with
            // kTFGrenadeRestitution, damp the tangential slide.
            for (int i = 0; i < 3; ++i)
            {
                const float tangential = g.vel[i] - normal[i] * vn;
                g.vel[i] = tangential * kTFGrenadeTangentDamping - normal[i] * vn * kTFGrenadeRestitution;
            }
        }
        if (WeaponMath::Len3(g.vel) < kTFGrenadeRestSpeedMps)
        {
            g.vel[0] = g.vel[1] = g.vel[2] = 0.0f;
            g.resting = true;
        }
    }

    void TFGrenadeSystem::ServerDetonate(const ServerGrenade& g)
    {
        ++m_detonations;
        if (!m_ctx)
            return;

        // loadout-depth wave: smoke/flash are status/presentation effects, not
        // damage — branch BEFORE the splash-damage pass (their weapons.json
        // rows already carry splashDamage 0, so the fallthrough below would
        // have no-op'd anyway; this also skips two ForEachAlivePawn sweeps for
        // the common frag case).
        if (g.effect == kTFGrenadeEffectSmoke)
        {
            ServerSpawnSmoke(g);
            return;
        }
        if (g.effect == kTFGrenadeEffectFlash)
        {
            ServerApplyFlash(g);
            return;
        }

        if (g.splashRadiusM <= 0.0f || g.splashDamage <= 0.0f || !m_ctx->players || !m_ctx->damage)
            return;

        // Pawn splash — the exact TFWeaponSystem::ExplodeAt recipe: linear
        // falloff to the blast edge, chest-height LOS through world statics.
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& pawn)
            {
                const float d = std::sqrt(WeaponMath::Dist2(pawn.pos, g.pos));
                if (d > g.splashRadiusM)
                    return;
                const float chest[3] = {pawn.pos[0], pawn.pos[1] + WeaponMath::kPawnHeightM * 0.5f, pawn.pos[2]};
                if (!Ballistics::SplashVisible(m_ctx->engine, g.pos, chest))
                    return;
                const float dmg = g.splashDamage * (1.0f - d / g.splashRadiusM);
                if (dmg > 1.0f)
                    m_ctx->damage->ServerApplyDamage(pawn.entity, g.throwerPawn, g.thrower, dmg, kDamageKindExplosive,
                                                     g.weapon, false);
            });

        if (m_ctx->deployables)
            m_ctx->deployables->ServerSplashDamageDeployables(g.pos, g.splashRadiusM, g.splashDamage, g.thrower);

        if (m_ctx->vehicles)
            m_ctx->vehicles->ServerApplySplash(g.pos, g.splashRadiusM, g.splashDamage, g.vsVehicleMult, g.throwerPawn,
                                               g.thrower, g.weapon, /*excludeVehicle*/ 0);
    }

    // ---------------------------------------------------------------------------
    // Broadcast (server -> everyone; local mirror fed directly)
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ServerBroadcast(uint16_t msgId, const void* payload, size_t size, bool reliable)
    {
        // Listen host / standalone: the local player is not a network client —
        // feed the mirror directly (TFAbilitySystem::ServerBroadcastState pattern).
        if (m_ctx && m_ctx->HasLocalPlayer() && m_ctx->role != NetRole::Client)
        {
            if (msgId == kTFMsgGrenadeSpawn && size == sizeof(TF_GrenadeSpawn))
            {
                TF_GrenadeSpawn msg{};
                std::memcpy(&msg, payload, sizeof(msg));
                ClientHandleSpawn(msg);
            }
            else if (msgId == kTFMsgGrenadeUpdate && size == sizeof(TF_GrenadeUpdate))
            {
                TF_GrenadeUpdate msg{};
                std::memcpy(&msg, payload, sizeof(msg));
                ClientHandleUpdate(msg);
            }
            else if (msgId == kTFMsgGrenadeBoom && size == sizeof(TF_GrenadeBoom))
            {
                TF_GrenadeBoom msg{};
                std::memcpy(&msg, payload, sizeof(msg));
                ClientHandleBoom(msg);
            }
            else if (msgId == kTFMsgSmokeSpawn && size == sizeof(TF_SmokeSpawn)) // loadout-depth wave
            {
                TF_SmokeSpawn msg{};
                std::memcpy(&msg, payload, sizeof(msg));
                ClientHandleSmoke(msg);
            }
        }

#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = reliable ? Spark::Net::ChannelType::Reliable : Spark::Net::ChannelType::Unreliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        for (const auto& [id, info] : nm.GetClients())
        {
            if (info.state == Spark::Net::ConnectionState::Connected)
                nm.SendToClient(id, msg);
        }
#else
        (void)reliable;
#endif
    }

    // ---------------------------------------------------------------------------
    // loadout-depth wave: smoke / flash detonation effects
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ServerSpawnSmoke(const ServerGrenade& g)
    {
        TF_SmokeSpawn msg{};
        msg.posQX = GrenadeDetail::QuantPos(g.pos[0]);
        msg.posQY = GrenadeDetail::QuantPos(g.pos[1]);
        msg.posQZ = GrenadeDetail::QuantPos(g.pos[2]);
        msg.radiusQ =
            static_cast<uint16_t>(std::lround(std::clamp(g.splashRadiusM, 0.0f, 8000.0f) * kTFGrenadePosScale));
        msg.durationMs = static_cast<uint16_t>(kTFSmokeLifeSec * 1000.0f);
        ServerBroadcast(kTFMsgSmokeSpawn, &msg, sizeof(msg), /*reliable*/ true);
    }

    void TFGrenadeSystem::ServerApplyFlash(const ServerGrenade& g)
    {
        if (!m_ctx->players || g.splashRadiusM <= 0.0f)
            return;
        const double now = NowSec();

        // Same LOS + linear-falloff recipe as splash damage (TFWeaponSystem::
        // ExplodeAt / this file's own damage pass above), but every visible
        // pawn in radius is affected regardless of faction — a flashbang
        // blinds anyone looking its way, thrower's team included.
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& pawn)
            {
                const float d = std::sqrt(WeaponMath::Dist2(pawn.pos, g.pos));
                if (d > g.splashRadiusM)
                    return;
                const float chest[3] = {pawn.pos[0], pawn.pos[1] + WeaponMath::kPawnHeightM * 0.5f, pawn.pos[2]};
                if (!Ballistics::SplashVisible(m_ctx->engine, g.pos, chest))
                    return;
                const float t = std::clamp(1.0f - d / g.splashRadiusM, 0.0f, 1.0f);
                if (t <= 0.02f)
                    return;

                const float durSec = kTFFlashMaxDurationSec * t;
                m_flashedUntil[pawn.owner] = now + durSec; // server truth (IsFlashed query)

                TF_FlashState msg{};
                msg.durationMs = static_cast<uint16_t>(std::lround(durSec * 1000.0f));
                msg.intensityQ = static_cast<uint8_t>(std::lround(t * 255.0f));
                SendToOwner(pawn.owner, kTFMsgFlashState, &msg, sizeof(msg));
            });
    }

    void TFGrenadeSystem::SendToOwner(PlayerId owner, uint16_t msgId, const void* payload, size_t size)
    {
        if (owner == kInvalidPlayer)
            return;

        // Listen host / standalone: feed the local mirror directly
        // (ServerBroadcast pattern above; TFProgressionSystem::SendToOwner
        // precedent for the unicast shape).
        if (m_ctx && m_ctx->HasLocalPlayer() && owner == m_ctx->localPlayer && m_ctx->role != NetRole::Client)
        {
            if (msgId == kTFMsgFlashState && size == sizeof(TF_FlashState))
            {
                TF_FlashState msg{};
                std::memcpy(&msg, payload, sizeof(msg));
                ClientHandleFlash(msg);
            }
        }

#ifdef ENABLE_NETWORKING
        if (!m_ctx || m_ctx->role == NetRole::Standalone)
            return;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(owner, msg);
#else
        (void)msgId;
        (void)payload;
        (void)size;
#endif
    }

    bool TFGrenadeSystem::IsFlashed(PlayerId player) const
    {
        const auto it = m_flashedUntil.find(player);
        return it != m_flashedUntil.end() && it->second > NowSec();
    }

} // namespace Terrafront
