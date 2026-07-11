/**
 * @file TFGrenadeSystem.cpp
 * @brief Frag grenades (W10) — server-authoritative throw/simulate/detonate,
 *        wire handlers, client mirror + G-key entry, sphere/boom rendering.
 *
 * See TFGrenadeSystem.h for the full lane design. Wire convention: C->S
 * TF_GrenadeThrow rides TFServerSim::RouteClientMessage (wiring snippet in
 * the wave report); S->C spawn/update/boom are broadcast by this system with
 * a direct local-mirror call for the listen-host/standalone player.
 */
#include "Game/TFGrenadeSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFBallistics.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "UI/TFChatWindow.h"
#include "UI/TFHUD.h"
#include "UI/TFLoginFlow.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSocialPanel.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFWorldSetup.h"

#include "Audio/AudioEngine.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Input/InputManager.h"
#include "Physics/PhysicsSystem.h" // engine umbrella header; stub-safe when Jolt is absent
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

namespace Terrafront
{

    namespace
    {
        constexpr uint8_t kDamageKindExplosive = 1; // TF_DamageEvent damageKind convention

        // Boom presentation (client only).
        constexpr float kBoomLifeSec = 0.45f;
        constexpr float kBoomSize0M = 1.0f;
        constexpr float kBoomSize1M = 3.8f;
        constexpr int kBoomFrames = 4;
        constexpr char kBoomSheet[] = "Assets/Textures/MMOFPS/fx/muzzle_flash_common.png";
        constexpr const char* kBoomAudio = "Audio/MMOFPS/vehicles/explosion.wav";
        constexpr float kBoomAudioRangeM = 90.0f;

        // Client-side mirror pruning: fuse + net slack. A grenade whose boom
        // packet was lost (unreliable updates can't revive it) dies here.
        constexpr float kClientStaleSec = kTFGrenadeFuseSec + 1.5f;

        /// TFWeaponSystem::BuildViewRay convention (yaw/pitch radians -> unit dir).
        void ViewDir(float yaw, float pitch, float out[3])
        {
            const float cp = std::cos(pitch);
            out[0] = cp * std::sin(yaw);
            out[1] = -std::sin(pitch);
            out[2] = cp * std::cos(yaw);
        }
    } // namespace

    TFGrenadeSystem::TFGrenadeSystem() = default;
    TFGrenadeSystem::~TFGrenadeSystem() = default;

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    bool TFGrenadeSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFGrenadeSystem initialized");
        return true;
    }

    void TFGrenadeSystem::Shutdown()
    {
        if (!m_initialized)
            return;

#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
#endif
        m_throwers.clear();
        m_live.clear();
        m_clientGrenades.clear();
        m_booms.clear();
        m_sphere.reset();
        m_quad.reset();
        m_initialized = false;
    }

    void TFGrenadeSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFAbilitySystem pattern: registered
        // after link-up so the real handler wins the per-type slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_clientGrenades.clear();
            m_booms.clear();
            m_localRemaining = -1;
        }
#endif

        if (m_ctx->HasLocalPlayer())
        {
            ClientReseedLocalCount();
            ClientPollThrowKey();
            ClientAdvance(deltaTime);
        }
    }

    void TFGrenadeSystem::FixedUpdate(float fixedDeltaTime)
    {
        m_clock += fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        m_updateAccum += fixedDeltaTime;
        const bool sendTick = m_updateAccum >= kTFGrenadeUpdatePeriodSec;
        if (sendTick)
            m_updateAccum -= kTFGrenadeUpdatePeriodSec;

        for (auto it = m_live.begin(); it != m_live.end();)
        {
            ServerGrenade& g = *it;
            g.lifeSec += fixedDeltaTime;

            if (g.lifeSec >= kTFGrenadeFuseSec)
            {
                ServerDetonate(g);
                TF_GrenadeBoom boom{};
                boom.grenadeId = g.id;
                boom.posQX = GrenadeDetail::QuantPos(g.pos[0]);
                boom.posQY = GrenadeDetail::QuantPos(g.pos[1]);
                boom.posQZ = GrenadeDetail::QuantPos(g.pos[2]);
                ServerBroadcast(kTFMsgGrenadeBoom, &boom, sizeof(boom), /*reliable*/ true);
                it = m_live.erase(it);
                continue;
            }

            const bool wasResting = g.resting;
            if (!g.resting)
                ServerStepGrenade(g, fixedDeltaTime);

            // Rest transitions get one reliable pin; airborne grenades get the
            // ~10 Hz unreliable correction stream.
            if ((g.resting && !wasResting) || (sendTick && !g.resting))
            {
                TF_GrenadeUpdate up{};
                up.grenadeId = g.id;
                up.posQX = GrenadeDetail::QuantPos(g.pos[0]);
                up.posQY = GrenadeDetail::QuantPos(g.pos[1]);
                up.posQZ = GrenadeDetail::QuantPos(g.pos[2]);
                up.velQX = GrenadeDetail::QuantVel(g.vel[0]);
                up.velQY = GrenadeDetail::QuantVel(g.vel[1]);
                up.velQZ = GrenadeDetail::QuantVel(g.vel[2]);
                up.flags = g.resting ? 1 : 0;
                ServerBroadcast(kTFMsgGrenadeUpdate, &up, sizeof(up), /*reliable*/ g.resting);
            }
            ++it;
        }
    }

    double TFGrenadeSystem::NowSec() const
    {
        if (m_ctx && m_ctx->IsAuthority() && m_ctx->serverSim)
            return m_ctx->serverSim->ServerTime();
        return m_clock;
    }

    const WeaponDef* TFGrenadeSystem::FragDef() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return nullptr;
        return m_ctx->data->GetWeaponByKey(kTFGrenadeWeaponKey);
    }

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
        if (m_live.size() >= kTFMaxLiveGrenades || !FragDef())
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

        const WeaponDef* def = FragDef();
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
        if (g.splashRadiusM <= 0.0f || g.splashDamage <= 0.0f || !m_ctx || !m_ctx->players || !m_ctx->damage)
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
    // Events (authority bookkeeping)
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        const ClassDef* cls = m_ctx->data ? m_ctx->data->GetClass(ev.cls) : nullptr;
        ThrowerRec& rec = m_throwers[ev.player];
        rec.remaining = cls ? cls->grenades : 0;
        rec.nextThrowAt = 0.0;
    }

    void TFGrenadeSystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        // Per-LIFE quota: the record dies with the pawn (respawn reseeds it).
        // Live grenades keep flying — a dead thrower's frag still detonates.
        m_throwers.erase(ev.victim);
    }

    // ---------------------------------------------------------------------------
    // Client: G key + count mirror
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ClientReseedLocalCount()
    {
        PawnInfo pi{};
        const bool alive = m_ctx->players && m_ctx->localPlayer != kInvalidPlayer &&
                           m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi) && pi.alive;
        if (alive && !m_wasAliveLocal && m_ctx->data && m_ctx->data->IsLoaded())
        {
            const ClassDef* cls = m_ctx->data->GetClass(pi.cls);
            m_localRemaining = cls ? cls->grenades : 0;
        }
        m_wasAliveLocal = alive;
    }

    void TFGrenadeSystem::ClientPollThrowKey()
    {
        if (!m_ctx || !m_ctx->InWorld() || m_ctx->localPlayer == kInvalidPlayer)
            return;
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;
        // Same input-suppression gate set as TFAbilitySystem::ClientPollAbilityKey.
        const bool uiOpen =
            (m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
            (m_ctx->loginFlow && m_ctx->loginFlow->IsOpen()) || (m_ctx->hud && m_ctx->hud->IsChatOpen()) ||
            (m_ctx->chatWindow && m_ctx->chatWindow->IsOpen()) || (m_ctx->socialPanel && m_ctx->socialPanel->IsOpen());
        if (uiOpen)
            return;
        PawnInfo pi{};
        if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi) || !pi.alive)
            return;
        if (!input->WasKeyPressed(m_throwVk))
            return;
        if (m_localRemaining == 0)
            return; // known-empty: don't spam the server (unknown (-1) still sends)

        SendThrow(pi.yaw, pi.pitch);
    }

    void TFGrenadeSystem::SendThrow(float viewYaw, float viewPitch)
    {
        if (!m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;
        if (m_ctx->IsAuthority())
        {
            // Mirror the RouteClientMessage enter-world gate for the direct
            // authority path (TFAbilitySystem::SendRequest defense-in-depth).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            float dir[3];
            ViewDir(viewYaw, viewPitch, dir);
            ServerTryThrow(m_ctx->localPlayer, dir);
        }
        else if (m_ctx->clientNet)
        {
            TF_GrenadeThrow req{};
            req.viewYaw = viewYaw;
            req.viewPitch = viewPitch;
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgGrenadeThrow), &req, sizeof(req));
        }
    }

    // ---------------------------------------------------------------------------
    // Client: mirror store + presentation state
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::ClientHandleSpawn(const TF_GrenadeSpawn& msg)
    {
        ClientGrenade& g = m_clientGrenades[msg.grenadeId];
        g.pos[0] = msg.posQX / kTFGrenadePosScale;
        g.pos[1] = msg.posQY / kTFGrenadePosScale;
        g.pos[2] = msg.posQZ / kTFGrenadePosScale;
        g.vel[0] = msg.velQX / kTFGrenadeVelScale;
        g.vel[1] = msg.velQY / kTFGrenadeVelScale;
        g.vel[2] = msg.velQZ / kTFGrenadeVelScale;
        g.resting = false;
        g.staleSec = 0.0f;

        if (m_ctx && msg.player == m_ctx->localPlayer)
            m_localRemaining = msg.remaining; // authoritative HUD count echo
    }

    void TFGrenadeSystem::ClientHandleUpdate(const TF_GrenadeUpdate& msg)
    {
        ClientGrenade& g = m_clientGrenades[msg.grenadeId]; // creates if the spawn raced
        g.pos[0] = msg.posQX / kTFGrenadePosScale;
        g.pos[1] = msg.posQY / kTFGrenadePosScale;
        g.pos[2] = msg.posQZ / kTFGrenadePosScale;
        g.vel[0] = msg.velQX / kTFGrenadeVelScale;
        g.vel[1] = msg.velQY / kTFGrenadeVelScale;
        g.vel[2] = msg.velQZ / kTFGrenadeVelScale;
        g.resting = (msg.flags & 1) != 0;
        g.staleSec = 0.0f;
    }

    void TFGrenadeSystem::ClientHandleBoom(const TF_GrenadeBoom& msg)
    {
        m_clientGrenades.erase(msg.grenadeId);

        BoomFx fx;
        fx.pos[0] = msg.posQX / kTFGrenadePosScale;
        fx.pos[1] = msg.posQY / kTFGrenadePosScale;
        fx.pos[2] = msg.posQZ / kTFGrenadePosScale;
        m_booms.push_back(fx);

        if (::AudioEngine* audio = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetAudio() : nullptr)
        {
            if (!m_boomAudioLoaded)
            {
                m_boomAudioLoaded = true;
                const std::string full = std::string("Assets/") + kBoomAudio;
                if (FAILED(audio->LoadSound(kBoomAudio, std::wstring(full.begin(), full.end()))))
                    SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] grenade boom audio load FAIL %s", kBoomAudio);
            }
            // Manual range gate on top of the 3D pan/attenuation so far-away
            // booms cost nothing.
            float lx = fx.pos[0], ly = fx.pos[1], lz = fx.pos[2];
            PawnInfo pi{};
            if (m_ctx->players && m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pi))
            {
                lx = pi.pos[0];
                ly = pi.pos[1];
                lz = pi.pos[2];
            }
            const float dx = fx.pos[0] - lx, dy = fx.pos[1] - ly, dz = fx.pos[2] - lz;
            if (dx * dx + dy * dy + dz * dz <= kBoomAudioRangeM * kBoomAudioRangeM)
                audio->PlaySound3D(kBoomAudio, DirectX::XMFLOAT3(fx.pos[0], fx.pos[1], fx.pos[2]), 0.9f);
        }
    }

    void TFGrenadeSystem::ClientAdvance(float dt)
    {
        if (dt <= 0.0f)
            return;

        for (auto& [id, g] : m_clientGrenades)
        {
            (void)id;
            g.staleSec += dt;
            if (g.resting)
                continue;
            // Extrapolate the arc between 10 Hz corrections with the same
            // gravity the server integrates; terrain-clamp so the body never
            // sinks while waiting for the next correction.
            g.vel[1] -= Ballistics::kProjectileGravityMps2 * dt;
            g.pos[0] += g.vel[0] * dt;
            g.pos[1] += g.vel[1] * dt;
            g.pos[2] += g.vel[2] * dt;
            if (m_ctx && m_ctx->world)
            {
                const float h = m_ctx->world->TerrainHeightAt(g.pos[0], g.pos[2]);
                if (g.pos[1] < h + kTFGrenadeRadiusM)
                    g.pos[1] = h + kTFGrenadeRadiusM;
            }
        }
        std::erase_if(m_clientGrenades, [](const auto& kv) { return kv.second.staleSec > kClientStaleSec; });

        for (BoomFx& b : m_booms)
            b.age += dt;
        std::erase_if(m_booms, [](const BoomFx& b) { return b.age >= kBoomLifeSec; });
    }

    // ---------------------------------------------------------------------------
    // Client: rendering (TFWorldSetup::RenderWorld hook)
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::RenderClientFx(GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                         const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!m_initialized || !gfx || !gfx->GetDevice() || !gfx->GetContext())
            return;
        if (m_clientGrenades.empty() && m_booms.empty())
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();

        // ------------------------------------------------------------ bodies
        if (!m_clientGrenades.empty())
        {
            if (!m_sphere)
            {
                m_sphere = std::make_unique<Mesh>();
                m_sphere->Initialize(gfx->GetDevice(), gfx->GetContext());
                m_sphere->CreateSphere(kTFGrenadeRadiusM, 10, 8);
            }
            if (m_sphere && m_sphere->GetIndexCount() > 0)
            {
                gfx->SetBasicShaders();
                gfx->SetBasicTexture(nullptr); // default 1x1 white; color below is the body tint
                for (const auto& [id, g] : m_clientGrenades)
                {
                    (void)id;
                    const XMMATRIX w = XMMatrixTranslation(g.pos[0], g.pos[1], g.pos[2]);
                    gfx->UpdateBasicConstants(w, view, proj, XMFLOAT4(0.10f, 0.10f, 0.12f, 1.0f), XMFLOAT2(1.0f, 1.0f));
                    m_sphere->Render(dc);
                }
            }
        }

        // ------------------------------------------------------------ booms
        if (!m_booms.empty())
        {
            if (!m_quad)
            {
                m_quad = std::make_unique<Mesh>();
                m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
                m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
            }
            if (m_quad && m_quad->GetIndexCount() > 0)
            {
                gfx->SetBasicShaders();
                gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(kBoomSheet));
                gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
                gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

                // Billboard basis from inv(view) rows (TFGroundFx recipe): local
                // X -> camera right, local Z -> camera up, local Y -> toward the
                // camera; determinant stays +1 for CreatePlane's winding.
                const XMMATRIX invView = XMMatrixInverse(nullptr, view);
                const XMVECTOR right = XMVector3Normalize(invView.r[0]);
                const XMVECTOR up = XMVector3Normalize(invView.r[1]);
                const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

                for (const BoomFx& b : m_booms)
                {
                    const float t = std::clamp(b.age / kBoomLifeSec, 0.0f, 1.0f);
                    const float size = kBoomSize0M + (kBoomSize1M - kBoomSize0M) * t;
                    const int frame = std::min(kBoomFrames - 1, static_cast<int>(t * kBoomFrames));
                    const float fade = (1.0f - t) * (1.0f - t); // ease-out

                    XMMATRIX bb = XMMatrixIdentity();
                    bb.r[0] = XMVectorScale(right, size);
                    bb.r[1] = toward;
                    bb.r[2] = XMVectorScale(up, size);
                    bb.r[3] = XMVectorSet(b.pos[0], b.pos[1], b.pos[2], 1.0f);

                    gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(6.0f, 4.5f, 2.0f, 1.0f),
                                              XMFLOAT2(1.0f / kBoomFrames, 1.0f), /*emissive*/ 1.0f, /*alpha*/ fade,
                                              XMFLOAT2(static_cast<float>(frame) / kBoomFrames, 0.0f));
                    m_quad->Render(dc);
                }

                gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
                gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
                gfx->SetBasicTexture(nullptr);
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Net handlers (pure clients; TFAbilitySystem lifecycle pattern)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFGrenadeSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFGrenadeSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgGrenadeSpawn),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_GrenadeSpawn))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_GrenadeSpawn msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleSpawn(msg);
                           });
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgGrenadeUpdate),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_GrenadeUpdate))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_GrenadeUpdate msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleUpdate(msg);
                           });
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgGrenadeBoom),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_GrenadeBoom))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_GrenadeBoom msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleBoom(msg);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] grenade mirror handlers registered");
    }

    void TFGrenadeSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with no-ops so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFMsgGrenadeSpawn, kTFMsgGrenadeUpdate, kTFMsgGrenadeBoom})
        {
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        }
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFGrenadeSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Grenades"))
            return;
        ImGui::Text("server live : %zu / %u", m_live.size(), kTFMaxLiveGrenades);
        ImGui::Text("throws      : %u ok, %u rejected", m_throws, m_rejected);
        ImGui::Text("detonations : %u", m_detonations);
        ImGui::Text("client view : %zu bodies, %zu booms", m_clientGrenades.size(), m_booms.size());
        ImGui::Text("local count : %d", m_localRemaining);
        ImGui::Text("bad packets : %u", m_badPackets);
#endif
    }

} // namespace Terrafront
