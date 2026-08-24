/**
 * @file TFWeatherFx.cpp
 * @brief Cindral dust-storm cycle (see TFWeatherFx.h for the ownership/wiring
 *        contract). Cycle is server-authoritative and mirrored via the 0x547C
 *        heartbeat; the client presentation (flake billboards + tint quad)
 *        lives in TFWeatherFxRender.cpp (same class, split per the repo
 *        file-size rules — mirrors the TFWorldSetup/-Render split).
 */
#include "World/TFWeatherFx.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include "Graphics/Mesh.h" // complete type for ~unique_ptr<Mesh> (Get's static instance)

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
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

        // Fog-in approximations (consumed via accessors; see header).
        constexpr float kStormCullScale = 0.40f; ///< decor cull ranges at full storm
        constexpr float kStormSkyDim = 0.55f;    ///< skybox tint multiplier at full storm

    } // namespace

    // ---------------------------------------------------------------------------

    TFWeatherFx& TFWeatherFx::Get()
    {
        static TFWeatherFx s_instance;
        return s_instance;
    }

    void TFWeatherFx::Shutdown()
    {
        if (m_debugCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_weather");
            m_debugCmd = false;
        }

#ifdef ENABLE_NETWORKING
        if (m_netHandler)
        {
            // NetworkManager has replacement semantics rather than an explicit
            // unregister API. Remove the callback that captures this before the
            // module DLL can unload.
            Spark::Net::NetworkManager::GetInstance().RegisterHandler(
                static_cast<Spark::Net::MessageType>(kTFMsgWeatherState), [](const Spark::Net::NetworkMessage&) {});
            m_netHandler = false;
        }
#endif

        m_ctx = nullptr;
        m_flakes.clear();
        m_quad.reset();
        m_phase = Phase::Clear;
        m_elapsedSec = 0.0f;
        m_durationSec = 900.0f;
        m_intensity = 0.0f;
        m_netClock = 0.0;
        m_nextBeat = 0.0;
        m_lastRealTime = -1.0;
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
            m_debugCmd = true;
        }
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

} // namespace Terrafront
