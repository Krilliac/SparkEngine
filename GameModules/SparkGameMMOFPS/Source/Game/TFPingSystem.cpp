/**
 * @file TFPingSystem.cpp
 * @brief Squad-scoped tactical pings (W11) — server validation + squad
 *        rebroadcast, client mirror, and the billboarded diamond markers.
 *
 * See TFPingSystem.h for the full lane design. Wire convention: C->S
 * TF_PingPlace rides TFServerSim::RouteClientMessage (wiring snippet in the
 * wave report); S->C TF_PingState is sent per squad member by this system with
 * a direct local-mirror call for the listen-host/standalone player.
 */
#include "Game/TFPingSystem.h"

#include "Game/TFAbilitySystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFSquadSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
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
        // Marker presentation (client only).
        constexpr float kMarkerBaseSizeM = 0.55f;  ///< diamond half-diagonal at close range
        constexpr float kMarkerDistScale = 0.012f; ///< extra size per meter (keeps it readable far away)
        constexpr float kMarkerMaxSizeM = 3.2f;    ///< size cap at long range
        constexpr float kMarkerBobAmpM = 0.14f;    ///< gentle vertical bob amplitude
        constexpr float kMarkerBobHz = 0.7f;       ///< bob cycles per second
        constexpr float kMarkerHeadM = 1.9f;       ///< Enemy pings anchor over the pawn head
        constexpr float kMarkerLiftM = 0.55f;      ///< Location/Support pings hover over the hit point

        constexpr float kSqrtHalf = 0.70710678f; // cos/sin 45 deg (diamond rotation)

        float MarkerAnchorY(TFPingType type, float posY)
        {
            return posY + (type == TFPingType::Enemy ? kMarkerHeadM : kMarkerLiftM);
        }
    } // namespace

    TFPingSystem::TFPingSystem() = default;
    TFPingSystem::~TFPingSystem() = default;

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    bool TFPingSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFPingSystem initialized");
        return true;
    }

    void TFPingSystem::Shutdown()
    {
        if (!m_initialized)
            return;

#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
#endif
        m_live.clear();
        m_nextPingAt.clear();
        m_clientPings.clear();
        m_quad.reset();
        m_initialized = false;
    }

    void TFPingSystem::Update(float deltaTime)
    {
        m_clock += deltaTime;
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
            m_clientPings.clear();
        }
#endif

        // Expiry is comms bookkeeping, not simulation — the Update tick is
        // precise enough and needs no Main.cpp FixedUpdate wiring.
        if (m_ctx->IsAuthority())
            ServerSweepExpired();

        if (m_ctx->HasLocalPlayer())
            ClientAdvance(deltaTime);
    }

    void TFPingSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // comms-only system; nothing simulates on the fixed tick
    }

    double TFPingSystem::NowSec() const
    {
        if (m_ctx && m_ctx->IsAuthority() && m_ctx->serverSim)
            return m_ctx->serverSim->ServerTime();
        return m_clock;
    }

    // ---------------------------------------------------------------------------
    // Server: placement validation + squad broadcast
    // ---------------------------------------------------------------------------

    void TFPingSystem::ServerHandlePingMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        if (size != sizeof(TF_PingPlace) || !data)
        {
            ++m_badPackets;
            return;
        }
        TF_PingPlace msg{};
        std::memcpy(&msg, data, sizeof(msg));
        if (msg.type >= static_cast<uint8_t>(TFPingType::COUNT) || !std::isfinite(msg.posX) ||
            !std::isfinite(msg.posY) || !std::isfinite(msg.posZ))
        {
            ++m_badPackets;
            return;
        }
        const float pos[3] = {msg.posX, msg.posY, msg.posZ};
        ServerTryPlace(sender, static_cast<TFPingType>(msg.type), pos, msg.targetEntity);
    }

    bool TFPingSystem::ServerTryPlace(PlayerId sender, TFPingType type, const float pos[3], EntityId targetEntity)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->players || sender == kInvalidPlayer)
            return false;

        PawnInfo me{};
        if (!m_ctx->players->GetPawnByPlayer(sender, me) || !me.alive)
        {
            ++m_rejected;
            return false;
        }

        const double now = NowSec();
        const auto rlIt = m_nextPingAt.find(sender);
        if (rlIt != m_nextPingAt.end() && now < rlIt->second)
        {
            ++m_rejected;
            return false;
        }

        ServerPing p;
        p.owner = sender;
        p.faction = me.faction;
        p.type = type;

        switch (type)
        {
        case TFPingType::Enemy:
        {
            // Anchor to the SERVER's view of the named pawn — never client data.
            PawnInfo target{};
            if (targetEntity == 0 || !m_ctx->players->GetPawnByEntity(targetEntity, target) || !target.alive ||
                target.faction == me.faction || target.faction == FactionId::None)
            {
                ++m_rejected;
                return false;
            }
            // A ping may never out a cloaked Ghost (TFNameplates parity).
            if (m_ctx->abilities && m_ctx->abilities->IsPawnVeiled(targetEntity))
            {
                ++m_rejected;
                return false;
            }
            if (std::sqrt(WeaponMath::Dist2(target.pos, me.pos)) > kTFPingMaxRangeM + kTFPingRangeSlackM)
            {
                ++m_rejected;
                return false;
            }
            p.pos[0] = target.pos[0];
            p.pos[1] = target.pos[1];
            p.pos[2] = target.pos[2];
            break;
        }
        case TFPingType::NeedSupport:
            // Always the placer's own pawn; the payload position is ignored.
            p.pos[0] = me.pos[0];
            p.pos[1] = me.pos[1];
            p.pos[2] = me.pos[2];
            break;
        default: // Location
            if (std::sqrt(WeaponMath::Dist2(pos, me.pos)) > kTFPingMaxRangeM + kTFPingRangeSlackM)
            {
                ++m_rejected;
                return false;
            }
            p.pos[0] = pos[0];
            p.pos[1] = pos[1];
            p.pos[2] = pos[2];
            break;
        }

        // Live cap per player: replace the oldest (modern-FPS convention).
        uint32_t mine = 0;
        size_t oldest = m_live.size();
        double oldestAt = 1.0e18;
        for (size_t i = 0; i < m_live.size(); ++i)
        {
            if (m_live[i].owner != sender)
                continue;
            ++mine;
            if (m_live[i].placedAt < oldestAt)
            {
                oldestAt = m_live[i].placedAt;
                oldest = i;
            }
        }
        if (mine >= kTFMaxLivePingsPerPlayer && oldest < m_live.size())
            ServerRemovePing(oldest);

        p.id = m_nextPingId++;
        if (m_nextPingId == 0)
            m_nextPingId = 1;
        p.squad = m_ctx->squads ? m_ctx->squads->SquadOf(sender) : kInvalidSquad;
        p.placedAt = now;
        p.expiresAt = now + (type == TFPingType::Enemy ? kTFPingEnemyLifetimeSec : kTFPingLifetimeSec);

        m_nextPingAt[sender] = now + kTFPingIntervalSec;
        ++m_placed;
        m_live.push_back(p);
        BroadcastState(p, /*op*/ 0);
        return true;
    }

    void TFPingSystem::ServerSweepExpired()
    {
        const double now = NowSec();
        for (size_t i = 0; i < m_live.size();)
        {
            if (m_live[i].expiresAt <= now)
            {
                ++m_expired;
                ServerRemovePing(i); // erases; do not advance
            }
            else
            {
                ++i;
            }
        }
    }

    void TFPingSystem::ServerRemovePing(size_t index)
    {
        if (index >= m_live.size())
            return;
        BroadcastState(m_live[index], /*op*/ 1);
        m_live.erase(m_live.begin() + static_cast<std::ptrdiff_t>(index));
    }

    void TFPingSystem::BroadcastState(const ServerPing& ping, uint8_t op)
    {
        TF_PingState msg{};
        msg.pingId = ping.id;
        msg.op = op;
        msg.type = static_cast<uint8_t>(ping.type);
        msg.owner = ping.owner;
        msg.posX = ping.pos[0];
        msg.posY = ping.pos[1];
        msg.posZ = ping.pos[2];
        msg.lifeSec = op == 0 ? static_cast<float>(std::max(0.0, ping.expiresAt - NowSec())) : 0.0f;
        msg.ownerFaction = static_cast<uint8_t>(ping.faction);

        ForEachRecipient(ping.owner, ping.squad, [&](PlayerId target) { SendStateTo(target, msg); });
    }

    void TFPingSystem::ForEachRecipient(PlayerId owner, SquadId squad, const std::function<void(PlayerId)>& fn) const
    {
        // Membership is evaluated at event time against the server registry
        // (kInvalidSquad == solo: owner only). The candidate set is the local
        // host player (never a socket client) plus every connected client.
        const auto visible = [&](PlayerId p)
        {
            if (p == owner)
                return true;
            return squad != kInvalidSquad && m_ctx->squads && m_ctx->squads->SquadOf(p) == squad;
        };

        if (m_ctx->HasLocalPlayer() && m_ctx->role != NetRole::Client && m_ctx->localPlayer != kInvalidPlayer &&
            visible(m_ctx->localPlayer))
        {
            fn(m_ctx->localPlayer);
        }

#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        for (const auto& [id, info] : nm.GetClients())
        {
            if (info.state == Spark::Net::ConnectionState::Connected && visible(id))
                fn(id);
        }
#endif
    }

    void TFPingSystem::SendStateTo(PlayerId target, const TF_PingState& msg)
    {
        // Listen host / standalone: the local player is not a network client —
        // feed the mirror directly (TFSquadSystem::SendEchoTo pattern).
        if (m_ctx && m_ctx->HasLocalPlayer() && target == m_ctx->localPlayer && m_ctx->role != NetRole::Client)
        {
            ClientHandleState(msg);
            return;
        }
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage net;
        net.type = static_cast<Spark::Net::MessageType>(kTFMsgPingState);
        net.channel = Spark::Net::ChannelType::Reliable;
        net.payload.resize(sizeof(msg));
        std::memcpy(net.payload.data(), &msg, sizeof(msg));
        nm.SendToClient(target, net);
#else
        (void)target;
#endif
    }

    // ---------------------------------------------------------------------------
    // Client: request routing + mirror store
    // ---------------------------------------------------------------------------

    void TFPingSystem::UiRequestPing(TFPingType type, const float pos[3], EntityId targetEntity)
    {
        if (!m_ctx || m_ctx->localPlayer == kInvalidPlayer || type >= TFPingType::COUNT || !pos)
            return;
        if (m_ctx->IsAuthority())
        {
            // Mirror the RouteClientMessage enter-world gate for the direct
            // authority path (TFGrenadeSystem::SendThrow defense-in-depth).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            ServerTryPlace(m_ctx->localPlayer, type, pos, targetEntity);
        }
        else if (m_ctx->clientNet)
        {
            TF_PingPlace req{};
            req.type = static_cast<uint8_t>(type);
            req.posX = pos[0];
            req.posY = pos[1];
            req.posZ = pos[2];
            req.targetEntity = targetEntity;
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgPingPlace), &req, sizeof(req));
        }
    }

    void TFPingSystem::ClientHandleState(const TF_PingState& msg)
    {
        if (msg.op != 0)
        {
            m_clientPings.erase(msg.pingId);
            return;
        }
        if (msg.type >= static_cast<uint8_t>(TFPingType::COUNT) || !std::isfinite(msg.posX) ||
            !std::isfinite(msg.posY) || !std::isfinite(msg.posZ) || !std::isfinite(msg.lifeSec))
        {
            ++m_badPackets;
            return;
        }
        ClientPing& p = m_clientPings[msg.pingId];
        p.type = static_cast<TFPingType>(msg.type);
        p.faction = msg.ownerFaction < static_cast<uint8_t>(FactionId::COUNT) ? static_cast<FactionId>(msg.ownerFaction)
                                                                              : FactionId::None;
        p.owner = msg.owner;
        p.pos[0] = msg.posX;
        p.pos[1] = msg.posY;
        p.pos[2] = msg.posZ;
        p.lifeLeft = std::clamp(msg.lifeSec, 0.0f, kTFPingLifetimeSec);
        p.age = 0.0f;
    }

    void TFPingSystem::ClientAdvance(float dt)
    {
        if (dt <= 0.0f || m_clientPings.empty())
            return;
        for (auto& [id, p] : m_clientPings)
        {
            (void)id;
            p.age += dt;
            p.lifeLeft -= dt;
        }
        // Local lifetime is the safety net for a lost remove packet.
        std::erase_if(m_clientPings, [](const auto& kv) { return kv.second.lifeLeft <= 0.0f; });
    }

    void TFPingSystem::ForEachActivePing(const std::function<void(const TFPingView&)>& fn) const
    {
        for (const auto& [id, p] : m_clientPings)
        {
            TFPingView v{};
            v.pingId = id;
            v.type = p.type;
            v.faction = p.faction;
            v.owner = p.owner;
            v.pos[0] = p.pos[0];
            v.pos[1] = p.pos[1];
            v.pos[2] = p.pos[2];
            v.lifeLeft = p.lifeLeft;
            v.age = p.age;
            fn(v);
        }
    }

    // ---------------------------------------------------------------------------
    // Client: world markers (TFWorldSetup::RenderWorld hook)
    // ---------------------------------------------------------------------------

    void TFPingSystem::RenderClientFx(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!m_initialized || !gfx || !gfx->GetDevice() || !gfx->GetContext() || m_clientPings.empty())
            return;

        if (!m_quad)
        {
            m_quad = std::make_unique<Mesh>();
            m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
        }
        if (!m_quad || m_quad->GetIndexCount() == 0)
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();

        gfx->SetBasicShaders();
        gfx->SetBasicTexture(nullptr); // default 1x1 white; the color below is the marker tint
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        // Billboard basis from inv(view) rows (TFGroundFx / TFGrenadeSystem boom
        // recipe), with the plane's local X/Z axes rotated 45 deg inside the
        // (right, up) screen plane so the quad reads as a diamond.
        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

        const XMVECTOR camPos = invView.r[3];

        for (const auto& [id, p] : m_clientPings)
        {
            (void)id;
            float col[4];
            PingColor(p.type, col);

            const float bob = std::sin(p.age * kMarkerBobHz * 2.0f * 3.14159265f) * kMarkerBobAmpM;
            const XMVECTOR anchor = XMVectorSet(p.pos[0], MarkerAnchorY(p.type, p.pos[1]) + bob, p.pos[2], 1.0f);

            const float dist = XMVectorGetX(XMVector3Length(XMVectorSubtract(anchor, camPos)));
            const float size = std::min(kMarkerBaseSizeM + dist * kMarkerDistScale, kMarkerMaxSizeM);
            const float fade = std::clamp(p.lifeLeft / kTFPingMarkerFadeSec, 0.0f, 1.0f);
            if (fade <= 0.01f)
                continue;

            const XMVECTOR axisX =
                XMVectorScale(XMVectorAdd(XMVectorScale(right, kSqrtHalf), XMVectorScale(up, kSqrtHalf)), size);
            const XMVECTOR axisZ =
                XMVectorScale(XMVectorAdd(XMVectorScale(right, -kSqrtHalf), XMVectorScale(up, kSqrtHalf)), size);

            XMMATRIX bb = XMMatrixIdentity();
            bb.r[0] = axisX;
            bb.r[1] = toward;
            bb.r[2] = axisZ;
            bb.r[3] = XMVectorSetW(anchor, 1.0f);

            gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(col[0] * 2.2f, col[1] * 2.2f, col[2] * 2.2f, 1.0f),
                                      XMFLOAT2(1.0f, 1.0f), /*emissive*/ 1.0f, /*alpha*/ 0.85f * fade,
                                      XMFLOAT2(0.0f, 0.0f));
            m_quad->Render(dc);
        }

        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

    // ---------------------------------------------------------------------------
    // Net handlers (pure clients; TFAbilitySystem lifecycle pattern)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFPingSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFPingSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgPingState),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_PingState))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_PingState msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               ClientHandleState(msg);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] ping mirror handler registered");
    }

    void TFPingSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgPingState),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFPingSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Pings"))
            return;
        ImGui::Text("server live : %zu", m_live.size());
        ImGui::Text("placed      : %u ok, %u rejected", m_placed, m_rejected);
        ImGui::Text("expired     : %u", m_expired);
        ImGui::Text("client view : %zu markers", m_clientPings.size());
        ImGui::Text("bad packets : %u", m_badPackets);
#endif
    }

} // namespace Terrafront
