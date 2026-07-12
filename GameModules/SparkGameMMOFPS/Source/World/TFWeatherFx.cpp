/**
 * @file TFWeatherFx.cpp
 * @brief Cindral dust-storm cycle + client presentation (see TFWeatherFx.h for
 *        the ownership/wiring contract). Cycle is server-authoritative and
 *        mirrored via the 0x547C heartbeat; visuals reuse the TFGroundFx
 *        flipbook billboard recipe plus one camera-locked alpha tint quad.
 */
#include "World/TFWeatherFx.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Terrafront
{

    namespace
    {

        // ------------------------------------------------------------- cycle
        // Clear 10-20 min -> Building 60 s -> Storm 3-5 min -> Clearing 45 s.
        constexpr float kClearMinSec = 600.0f;
        constexpr float kClearMaxSec = 1200.0f;
        constexpr float kBuildingSec = 60.0f;
        constexpr float kStormMinSec = 180.0f;
        constexpr float kStormMaxSec = 300.0f;
        constexpr float kClearingSec = 45.0f;

        /// Full-swing smoothing time for the presentation intensity (packet
        /// snaps and phase edges never pop; the envelope itself ramps slowly).
        constexpr float kIntensitySmoothSec = 4.0f;

        // ------------------------------------------------------------ visuals
        constexpr float kMaxFrameDtSec = 0.05f; ///< clamp render dt (alt-tab / hitches)

        // Wind-blown dust flakes (TFGroundFx recipe, storm-scaled count).
        constexpr size_t kStormMaxFlakes = 120;  ///< hard cap at full intensity
        constexpr int kMaxSpawnPerFrame = 6;     ///< population ramps in, never bursts
        constexpr float kFlakeMinRadiusM = 6.0f; ///< spawn ring around the camera
        constexpr float kFlakeMaxRadiusM = 28.0f;
        constexpr float kFlakeYBelowM = 3.0f; ///< spawn band relative to camera height
        constexpr float kFlakeYAboveM = 7.0f;
        constexpr float kFlakeKillDistM = 60.0f;               ///< blown past this from the camera -> recycle
        constexpr float kWindBaseMps = 10.0f;                  ///< horizontal wind at intensity 0
        constexpr float kWindStormMps = 8.0f;                  ///< ... plus this at intensity 1
        constexpr float kFlakeAlpha = 0.28f;                   ///< peak additive weight at full storm
        constexpr float kFlakeColor[3] = {1.0f, 0.86f, 0.62f}; ///< sandy, matches hover dust

        // Flipbook (same 4-frame strip as the hover dust — no new assets).
        constexpr int kDustFrames = 4;
        constexpr char kDustSheet[] = "Assets/Textures/MMOFPS/fx/hover_dust.png";

        // Fullscreen dust tint (alpha quad locked to the camera).
        constexpr float kTintDistM = 0.35f;    ///< quad distance in front of the camera
        constexpr float kTintMargin = 1.10f;   ///< overscan so FOV edges never peek
        constexpr float kTintAlphaMax = 0.20f; ///< subtle wash at full storm
        constexpr float kTintColor[3] = {0.76f, 0.58f, 0.36f};

        // Fog-in approximations (consumed via accessors; see header).
        constexpr float kStormCullScale = 0.40f; ///< decor cull ranges at full storm
        constexpr float kStormSkyDim = 0.55f;    ///< skybox tint multiplier at full storm

        double NowRealSec()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

    } // namespace

    // ---------------------------------------------------------------------------

    TFWeatherFx& TFWeatherFx::Get()
    {
        static TFWeatherFx s_instance;
        return s_instance;
    }

    float TFWeatherFx::Rand01()
    {
        return static_cast<float>(m_rng() & 0xFFFFu) / 65535.0f;
    }

    const char* TFWeatherFx::PhaseName(Phase p)
    {
        switch (p)
        {
        case Phase::Clear:
            return "clear";
        case Phase::Building:
            return "building";
        case Phase::Storm:
            return "storm";
        case Phase::Clearing:
            return "clearing";
        default:
            return "?";
        }
    }

    float TFWeatherFx::DecorCullScale() const
    {
        return 1.0f + (kStormCullScale - 1.0f) * m_intensity;
    }

    float TFWeatherFx::SkyboxDim() const
    {
        return 1.0f + (kStormSkyDim - 1.0f) * m_intensity;
    }

    // ---------------------------------------------------------------------------
    // Update (all roles)
    // ---------------------------------------------------------------------------

    void TFWeatherFx::Update(TFGameContext& ctx, float deltaTime)
    {
        m_ctx = &ctx;
        EnsureConsoleCommand();

        const float dt = std::clamp(deltaTime, 0.0f, 0.25f);
        m_netClock += dt;

        if (ctx.IsAuthority())
        {
            AdvanceCycle(dt);
        }
        else
        {
            ClientPollHandler(ctx);
            // Extrapolate the mirrored phase between heartbeats — the envelope
            // clamps at the phase end and the transition arrives reliably, so
            // the worst drift is one heartbeat of a slow ramp.
            m_elapsedSec += dt;
        }

        // Shared smoothing toward the raw envelope (~full swing over
        // kIntensitySmoothSec) — every consumer reads m_intensity only.
        const float raw = PhaseEnvelope();
        const float step = dt / kIntensitySmoothSec;
        m_intensity += std::clamp(raw - m_intensity, -step, step);
        m_intensity = std::clamp(m_intensity, 0.0f, 1.0f);
    }

    void TFWeatherFx::AdvanceCycle(float dt)
    {
        m_elapsedSec += dt;
        bool transitioned = false;
        if (m_elapsedSec >= m_durationSec)
        {
            const Phase next =
                static_cast<Phase>((static_cast<uint8_t>(m_phase) + 1u) % static_cast<uint8_t>(Phase::COUNT));
            EnterPhase(next);
            transitioned = true;
        }
        ServerMaybeBroadcast(transitioned);
    }

    void TFWeatherFx::EnterPhase(Phase p)
    {
        m_phase = p;
        m_elapsedSec = 0.0f;
        m_durationSec = RollDurationSec(p);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] weather -> %s (%.0f s)", PhaseName(p),
                       static_cast<double>(m_durationSec));
    }

    float TFWeatherFx::RollDurationSec(Phase p)
    {
        switch (p)
        {
        case Phase::Clear:
            return kClearMinSec + (kClearMaxSec - kClearMinSec) * Rand01();
        case Phase::Building:
            return kBuildingSec;
        case Phase::Storm:
            return kStormMinSec + (kStormMaxSec - kStormMinSec) * Rand01();
        case Phase::Clearing:
        default:
            return kClearingSec;
        }
    }

    float TFWeatherFx::PhaseEnvelope() const
    {
        const float t = (m_durationSec > 0.0f) ? std::clamp(m_elapsedSec / m_durationSec, 0.0f, 1.0f) : 1.0f;
        switch (m_phase)
        {
        case Phase::Building:
            return t;
        case Phase::Storm:
            return 1.0f;
        case Phase::Clearing:
            return 1.0f - t;
        case Phase::Clear:
        default:
            return 0.0f;
        }
    }

    // ---------------------------------------------------------------------------
    // Console (tf_weather — authority forces a phase; everyone gets status)
    // ---------------------------------------------------------------------------

    void TFWeatherFx::EnsureConsoleCommand()
    {
        if (m_debugCmd)
            return;
        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_weather"))
        {
            console.RegisterCommand(
                "tf_weather",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!args.empty())
                    {
                        if (!m_ctx || !m_ctx->IsAuthority())
                            return "[TF] weather is server-owned — phase can only be forced on the host";
                        Phase p;
                        if (args[0] == "clear")
                            p = Phase::Clear;
                        else if (args[0] == "building")
                            p = Phase::Building;
                        else if (args[0] == "storm")
                            p = Phase::Storm;
                        else if (args[0] == "clearing")
                            p = Phase::Clearing;
                        else
                            return "[TF] usage: tf_weather [clear|building|storm|clearing]";
                        EnterPhase(p);
                        ServerMaybeBroadcast(true);
                    }
                    char buf[192];
                    std::snprintf(
                        buf, sizeof(buf), "[TF] weather: %s  %.0f/%.0f s  intensity %.2f (raw %.2f)  flakes %zu",
                        PhaseName(m_phase), static_cast<double>(m_elapsedSec), static_cast<double>(m_durationSec),
                        static_cast<double>(m_intensity), static_cast<double>(PhaseEnvelope()), m_flakes.size());
                    return std::string(buf);
                },
                "Dust-storm status; on the host, force a phase: tf_weather [clear|building|storm|clearing]",
                "TERRAFRONT", "tf_weather [phase]");
        }
        m_debugCmd = true;
    }

    // ---------------------------------------------------------------------------
    // Net sync (0x547C)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    void TFWeatherFx::ServerMaybeBroadcast(bool transition)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        if (!transition && m_netClock < m_nextBeat)
            return;
        m_nextBeat = m_netClock + kWeatherSyncSec;

        TF_WeatherState st{};
        st.phase = static_cast<uint8_t>(m_phase);
        st.intensityQ = static_cast<uint8_t>(std::lround(std::clamp(PhaseEnvelope(), 0.0f, 1.0f) * 255.0f));
        st.elapsedDs = static_cast<uint16_t>(std::min(65535L, std::lround(m_elapsedSec * 10.0f)));
        st.durationDs = static_cast<uint16_t>(std::min(65535L, std::lround(m_durationSec * 10.0f)));

        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFMsgWeatherState);
        // Transitions ride reliable so phase edges are never missed; the 2 s
        // heartbeat is unreliable (a dropped beat converges on the next one).
        msg.channel = transition ? Spark::Net::ChannelType::Reliable : Spark::Net::ChannelType::Unreliable;
        msg.payload.resize(sizeof(st));
        std::memcpy(msg.payload.data(), &st, sizeof(st));
        nm.SendToAll(msg);
    }

    void TFWeatherFx::ClientPollHandler(TFGameContext& ctx)
    {
        // TFSocialSystem poll pattern (see TFAudioAmbience): register while the
        // client connection is live, replace with a no-op when it drops so no
        // dangling `this` survives the module DLL.
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const bool clientUp = ctx.role == NetRole::Client && nm.IsInitialized() &&
                              nm.GetRole() == Spark::Net::NetworkRole::Client &&
                              nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
        if (clientUp && !m_netHandler)
        {
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgWeatherState),
                               [this](const Spark::Net::NetworkMessage& m)
                               { OnNetWeatherState(m.payload.data(), m.payload.size()); });
            m_netHandler = true;
        }
        else if (!clientUp && m_netHandler)
        {
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgWeatherState),
                               [](const Spark::Net::NetworkMessage&) {});
            m_netHandler = false;
        }
    }

    void TFWeatherFx::OnNetWeatherState(const void* data, size_t size)
    {
        if (size != sizeof(TF_WeatherState))
            return; // malformed — drop
        TF_WeatherState st;
        std::memcpy(&st, data, sizeof(st));
        if (st.phase >= static_cast<uint8_t>(Phase::COUNT))
            return; // future/bad phase — drop, keep extrapolating
        m_phase = static_cast<Phase>(st.phase);
        m_elapsedSec = static_cast<float>(st.elapsedDs) * 0.1f;
        m_durationSec = std::max(0.1f, static_cast<float>(st.durationDs) * 0.1f);
        // Intensity stays smoothed locally (Update); intensityQ is debug-only.
    }

#else // !ENABLE_NETWORKING — standalone builds run the cycle locally, no sync

    void TFWeatherFx::ServerMaybeBroadcast(bool transition)
    {
        (void)transition;
    }

    void TFWeatherFx::ClientPollHandler(TFGameContext& ctx)
    {
        (void)ctx;
    }

    void TFWeatherFx::OnNetWeatherState(const void* data, size_t size)
    {
        (void)data;
        (void)size;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Render (client presentation)
    // ---------------------------------------------------------------------------

    void TFWeatherFx::Render(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                             const DirectX::XMMATRIX& proj)
    {
        if (!gfx || !gfx->GetDevice() || !gfx->GetContext() || !ctx.HasLocalPlayer())
            return;

        const double nowReal = NowRealSec();
        float dt = (m_lastRealTime < 0.0) ? 0.0f : static_cast<float>(nowReal - m_lastRealTime);
        m_lastRealTime = nowReal;
        dt = std::clamp(dt, 0.0f, kMaxFrameDtSec);

        if (m_intensity <= 0.003f && m_flakes.empty())
            return; // clear sky and nothing left to fade out

        if (!m_quad)
        {
            m_quad = std::make_unique<Mesh>();
            m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
        }
        if (!m_quad || m_quad->GetIndexCount() == 0)
            return;

        UpdateAndDrawFlakes(ctx, gfx, view, proj, dt);
        if (m_intensity > 0.003f)
            DrawScreenTint(gfx, view, proj);

        // Restore the frame-baseline states for whoever draws next.
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

    void TFWeatherFx::UpdateAndDrawFlakes(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                          const DirectX::XMMATRIX& proj, float dt)
    {
        using namespace DirectX;
        (void)ctx;

        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR camPos = invView.r[3];
        const float camX = XMVectorGetX(camPos);
        const float camY = XMVectorGetY(camPos);
        const float camZ = XMVectorGetZ(camPos);

        // Slowly wandering world-space wind heading — reads as gusty without
        // any per-flake noise field.
        m_windAngle += dt * (0.02f + 0.06f * std::sin(static_cast<float>(m_netClock) * 0.13f));
        const float windX = std::sin(m_windAngle);
        const float windZ = std::cos(m_windAngle);
        const float windMps = kWindBaseMps + kWindStormMps * m_intensity;

        // ------------------------------------------------------------ spawn
        const size_t target = static_cast<size_t>(std::lround(static_cast<double>(kStormMaxFlakes) * m_intensity));
        int spawned = 0;
        while (m_flakes.size() < target && spawned++ < kMaxSpawnPerFrame)
        {
            DustFlake f;
            const float ang = Rand01() * 6.2831853f;
            const float rad = kFlakeMinRadiusM + (kFlakeMaxRadiusM - kFlakeMinRadiusM) * Rand01();
            f.pos[0] = camX + std::cos(ang) * rad;
            f.pos[2] = camZ + std::sin(ang) * rad;
            f.pos[1] = camY - kFlakeYBelowM + (kFlakeYBelowM + kFlakeYAboveM) * Rand01();
            const float speed = windMps * (0.7f + 0.6f * Rand01());
            f.vel[0] = windX * speed + (Rand01() - 0.5f) * 3.0f;
            f.vel[2] = windZ * speed + (Rand01() - 0.5f) * 3.0f;
            f.vel[1] = (Rand01() - 0.5f) * 1.2f;
            f.life = 1.4f + 1.4f * Rand01();
            f.size = 1.1f + 1.6f * Rand01();
            m_flakes.push_back(f);
        }

        // ---------------------------------------------------------- advance
        for (DustFlake& f : m_flakes)
        {
            f.age += dt;
            f.pos[0] += f.vel[0] * dt;
            f.pos[1] += f.vel[1] * dt;
            f.pos[2] += f.vel[2] * dt;
        }
        std::erase_if(m_flakes,
                      [&](const DustFlake& f)
                      {
                          if (f.age >= f.life)
                              return true;
                          const float dx = f.pos[0] - camX;
                          const float dz = f.pos[2] - camZ;
                          return dx * dx + dz * dz > kFlakeKillDistM * kFlakeKillDistM;
                      });
        // Over-target population (intensity fading down) is not force-culled —
        // flakes simply age out, so the storm dissolves instead of popping.
        if (m_flakes.empty())
            return;

        // ------------------------------------------------------------ render
        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(kDustSheet));
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly); // test, never write

        // Billboard basis (TFGroundFx recipe): local X -> camera right, local
        // Z -> camera up, local Y (front face) -> toward the camera.
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

        for (const DustFlake& f : m_flakes)
        {
            const float t = std::clamp(f.age / f.life, 0.0f, 1.0f);
            const int frame = std::min(kDustFrames - 1, static_cast<int>(t * kDustFrames));
            // Mid-life peak (4t(1-t)): flakes fade in, stream, fade out.
            const float fade = kFlakeAlpha * m_intensity * (4.0f * t * (1.0f - t));

            XMMATRIX bb = XMMatrixIdentity();
            bb.r[0] = XMVectorScale(right, f.size);
            bb.r[1] = toward;
            bb.r[2] = XMVectorScale(up, f.size);
            bb.r[3] = XMVectorSet(f.pos[0], f.pos[1], f.pos[2], 1.0f);

            gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(kFlakeColor[0], kFlakeColor[1], kFlakeColor[2], 1.0f),
                                      {1.0f / kDustFrames, 1.0f}, /*emissive*/ 1.0f, /*alpha*/ fade,
                                      XMFLOAT2(static_cast<float>(frame) / kDustFrames, 0.0f));
            m_quad->Render(dc);
        }
    }

    void TFWeatherFx::DrawScreenTint(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR forward = XMVector3Normalize(invView.r[2]); // camera look direction
        const XMVECTOR center = XMVectorAdd(invView.r[3], XMVectorScale(forward, kTintDistM));

        // Exact frustum extents at kTintDistM from the projection diagonal
        // (proj._11 = 1/(aspect*tan(fov/2)), proj._22 = 1/tan(fov/2)), plus a
        // margin so edges never peek at extreme aspect ratios.
        const float p00 = XMVectorGetX(proj.r[0]);
        const float p11 = XMVectorGetY(proj.r[1]);
        if (p00 <= 0.0f || p11 <= 0.0f)
            return; // not a perspective projection we understand — skip the tint
        const float fullW = 2.0f * (kTintDistM / p00) * kTintMargin;
        const float fullH = 2.0f * (kTintDistM / p11) * kTintMargin;

        XMMATRIX world = XMMatrixIdentity();
        world.r[0] = XMVectorScale(right, fullW);
        world.r[1] = XMVectorNegate(forward); // plane +Y normal faces the camera
        world.r[2] = XMVectorScale(up, fullH);
        world.r[3] = XMVectorSetW(center, 1.0f);

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(nullptr); // 1x1 white — pure color quad
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Alpha);
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        const float alpha = kTintAlphaMax * m_intensity;
        gfx->UpdateBasicConstants(world, view, proj, XMFLOAT4(kTintColor[0], kTintColor[1], kTintColor[2], 1.0f),
                                  XMFLOAT2(1.0f, 1.0f), /*emissive*/ 1.0f, /*alpha*/ alpha, XMFLOAT2(0.0f, 0.0f));
        m_quad->Render(dc);
    }

} // namespace Terrafront
