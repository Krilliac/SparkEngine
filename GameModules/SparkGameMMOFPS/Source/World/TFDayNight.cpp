/**
 * @file TFDayNight.cpp
 * @brief Continent day/night cycle: authoritative 40-minute day clock,
 *        TF_TimeOfDay wire sync (join burst + 30 s drift correction), the
 *        Spark::TimeOfDaySystem curve drive, and the remap onto the basic
 *        render path's frame lighting. See TFDayNight.h for the full design
 *        note.
 */
#include "World/TFDayNight.h"

#include "World/TFSanctuaryZone.h"

#include "Engine/World/TimeOfDaySystem.h"
#include "Graphics/GraphicsEngine.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Terrafront
{

    namespace
    {
        DirectX::XMFLOAT3 Lerp3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t)
        {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        }

        DirectX::XMFLOAT3 Norm3(float x, float y, float z)
        {
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len < 0.0001f)
                return {0.0f, -1.0f, 0.0f};
            return {x / len, y / len, z / len};
        }

        /// Push through the GraphicsEngine::SetEnvironmentLighting seam when the
        /// engine-side wiring has landed; compile-time no-op (false) before it
        /// has. The template parameter keeps the member lookup dependent, so
        /// this file builds against both engine states (C++23 requires-expr).
        template <typename Gfx>
        bool TrySetEnvLighting(Gfx* gfx, const DirectX::XMFLOAT3& lightDir, const DirectX::XMFLOAT3& lightColor,
                               float lightIntensity, const DirectX::XMFLOAT3& ambientColor, float ambientIntensity)
        {
            if constexpr (requires {
                              gfx->SetEnvironmentLighting(lightDir, lightColor, lightIntensity, ambientColor,
                                                          ambientIntensity);
                          })
            {
                gfx->SetEnvironmentLighting(lightDir, lightColor, lightIntensity, ambientColor, ambientIntensity);
                return true;
            }
            else
            {
                (void)gfx;
                (void)lightDir;
                (void)lightColor;
                (void)lightIntensity;
                (void)ambientColor;
                (void)ambientIntensity;
                return false;
            }
        }

        /// Parse "0.42" (day fraction) or "HH:MM" into a day fraction.
        bool ParseTimeArg(const std::string& arg, float& outFrac)
        {
            const size_t colon = arg.find(':');
            if (colon != std::string::npos)
            {
                int h = 0;
                int m = 0;
                if (std::sscanf(arg.c_str(), "%d:%d", &h, &m) != 2)
                    return false;
                if (h < 0 || h > 23 || m < 0 || m > 59)
                    return false;
                outFrac = (static_cast<float>(h) + static_cast<float>(m) / 60.0f) / 24.0f;
                return true;
            }
            char* end = nullptr;
            const float f = std::strtof(arg.c_str(), &end);
            if (end == arg.c_str() || f < 0.0f || f >= 1.0f)
                return false;
            outFrac = f;
            return true;
        }

        const char* PeriodName(float dayFrac)
        {
            const float h = dayFrac * 24.0f;
            if (h < 5.0f)
                return "night";
            if (h < 7.0f)
                return "dawn";
            if (h < 17.0f)
                return "day";
            if (h < 19.0f)
                return "dusk";
            return "night";
        }
    } // namespace

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    TFDayNight::TFDayNight() = default;
    TFDayNight::~TFDayNight()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDayNight::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_dayFrac = kTFDayStartFrac;
        m_syncAccum = 0.0f;
        m_mirrorValid = false;

        // Console self-registration (TFAlertSystem tf_alert pattern — no
        // TFCommands.cpp edit needed).
        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_time"))
        {
            console.RegisterCommand(
                "tf_time",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] day/night system not ready";
                    if (args.empty() || args[0] == "status" || args[0] == "get")
                        return StatusString();
                    if (args[0] == "set")
                    {
                        if (args.size() < 2)
                            return "[TF] usage: tf_time set <dayFrac 0..1 | HH:MM>";
                        float frac = 0.0f;
                        if (!ParseTimeArg(args[1], frac))
                            return "[TF] tf_time set: could not parse '" + args[1] + "' (dayFrac 0..1 or HH:MM)";
                        if (!ServerSetDayFrac(frac))
                            return "[TF] tf_time set: authority roles only";
                        return "[TF] time set to " + ClockString(frac);
                    }
                    return "[TF] usage: tf_time [set <dayFrac|HH:MM>]";
                },
                "Continent day/night clock: show or set the time of day", "TERRAFRONT",
                "tf_time [set <dayFrac 0..1 | HH:MM>]");
            m_consoleCmd = true;
        }

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDayNight initialized (day=%.0fs, boot time %s)",
                       static_cast<double>(kTFDayLengthSec), ClockString(m_dayFrac).c_str());
        return true;
    }

    void TFDayNight::Shutdown()
    {
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
        m_knownClients.clear();
#endif
        if (m_consoleCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_time");
            m_consoleCmd = false;
        }
        m_mirrorValid = false;
        m_initialized = false;
    }

    void TFDayNight::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

#ifdef ENABLE_NETWORKING
        // Client mirror handler lifecycle (TFMedalSystem pattern: registered
        // after link-up so the real handler wins the per-type slot).
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            m_mirrorValid = false;
        }
#endif

        if (m_ctx->IsAuthority())
        {
            m_dayFrac = WrapFrac(m_dayFrac + kTFDayRatePerSec * deltaTime);
            m_syncAccum += deltaTime;
            if (m_syncAccum >= kTFTimeSyncSec)
            {
                m_syncAccum = 0.0f;
                BroadcastTime();
            }
#ifdef ENABLE_NETWORKING
            PollJoins();
#endif
        }
        else if (m_mirrorValid)
        {
            // Free-run the mirror between syncs at the wire rate; the 30 s
            // server refresh snaps any drift.
            m_mirror.dayFrac = WrapFrac(m_mirror.dayFrac + m_mirror.rate * deltaTime);
        }

        if (m_ctx->HasLocalPlayer())
        {
            TF_TimeOfDay view{};
            GetView(view);
            DriveEngineClock(view, deltaTime);
            ApplyVisuals();
        }
    }

    void TFDayNight::FixedUpdate(float)
    {
        // Nothing simulation-facing lives here (determinism contract): the clock
        // is wall-clock server bookkeeping in Update() and lighting is purely
        // presentational.
    }

    // ---------------------------------------------------------------------------
    // Clock
    // ---------------------------------------------------------------------------

    float TFDayNight::WrapFrac(float f)
    {
        f = std::fmod(f, 1.0f);
        return f < 0.0f ? f + 1.0f : f;
    }

    void TFDayNight::GetView(TF_TimeOfDay& out) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            out.dayFrac = m_dayFrac;
            out.rate = kTFDayRatePerSec;
            return;
        }
        if (m_mirrorValid)
        {
            out = m_mirror;
            return;
        }
        // Pure client before the first sync: show the canonical boot time so
        // the world never flashes engine-default noon.
        out.dayFrac = kTFDayStartFrac;
        out.rate = kTFDayRatePerSec;
    }

    float TFDayNight::DayFrac() const
    {
        TF_TimeOfDay v{};
        GetView(v);
        return v.dayFrac;
    }

    bool TFDayNight::IsNight() const
    {
        const float h = DayFrac() * 24.0f;
        return h < 6.0f || h >= 18.0f; // sun below the horizon on the 6/18 curve
    }

    bool TFDayNight::ServerSetDayFrac(float dayFrac)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return false;
        m_dayFrac = WrapFrac(dayFrac);
        m_syncAccum = 0.0f;
        BroadcastTime();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] time of day set to %s (dayFrac %.3f)",
                       ClockString(m_dayFrac).c_str(), static_cast<double>(m_dayFrac));
        return true;
    }

    std::string TFDayNight::ClockString(float dayFrac)
    {
        const float hour = WrapFrac(dayFrac) * 24.0f;
        const int h = static_cast<int>(hour);
        const int m = static_cast<int>((hour - static_cast<float>(h)) * 60.0f);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        return std::string(buf);
    }

    std::string TFDayNight::StatusString() const
    {
        TF_TimeOfDay v{};
        GetView(v);
        std::ostringstream os;
        os << "[TF] time: " << ClockString(v.dayFrac) << " (" << PeriodName(v.dayFrac) << ", dayFrac ";
        char frac[16];
        std::snprintf(frac, sizeof(frac), "%.3f", static_cast<double>(v.dayFrac));
        os << frac << ", day length " << static_cast<int>(kTFDayLengthSec / 60.0f) << " min)";
        if (m_ctx && m_ctx->IsAuthority())
            os << " [authority]";
        else
            os << (m_mirrorValid ? " [mirror]" : " [mirror: no sync yet]");
        if (m_sanctuaryOverride)
            os << " sanctuary golden-hour override active";
        if (!m_seamPresent && m_ctx && m_ctx->HasLocalPlayer())
            os << " (engine lighting seam NOT wired - visuals inert)";
        return os.str();
    }

    // ---------------------------------------------------------------------------
    // Engine clock drive + visuals
    // ---------------------------------------------------------------------------

    void TFDayNight::DriveEngineClock(const TF_TimeOfDay& view, float dt)
    {
        auto& tod = Spark::TimeOfDaySystem::GetInstance();

        // Keep the engine clock free-running in lockstep with dayFrac: rate is
        // day-fractions/sec, the engine wants game-seconds per real second.
        tod.SetPaused(view.rate <= 0.0f);
        tod.SetTimeScale(view.rate * 86400.0f);
        tod.Update(dt);

        // Snap only on genuine divergence (join, tf_time set, long hitch) —
        // SetTimeOfDay logs at INFO, so per-frame calls would spam the log.
        const float targetHour = WrapFrac(view.dayFrac) * 24.0f;
        float delta = std::fabs(tod.GetTimeOfDay() - targetHour);
        if (delta > 12.0f)
            delta = 24.0f - delta;
        if (delta > kTFTodSnapHours)
        {
            tod.SetTimeOfDay(targetHour);
            ++m_engineSnaps;
        }
    }

    void TFDayNight::ApplyVisuals()
    {
        using DirectX::XMFLOAT3;

        GraphicsEngine* gfx = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetGraphics() : nullptr;
        if (!gfx)
            return; // headless (dedicated server): nothing to light

        // Zone check mirrors TFWorldSetup::DrawSky's per-zone sky selection —
        // both key off the camera position, so sky and lighting always agree.
        const XMFLOAT3 cam = gfx->GetFrameCameraPosition();
        m_sanctuaryOverride = TFTravel_IsInSanctuary(cam.x, cam.z);

        XMFLOAT3 lightDir;
        XMFLOAT3 lightColor;
        XMFLOAT3 ambientColor;
        float lightIntensity;
        float ambientIntensity;

        if (m_sanctuaryOverride)
        {
            // Frozen golden hour (kTFSanctuaryHour == 17:30, warmed): low western
            // sun, long warm light, bright readable ambient. Constants evaluated
            // from the TimeOfDaySystem curve at 17.5 h, then pushed warm.
            lightDir = XMFLOAT3(0.904f, -0.120f, 0.410f);
            lightColor = XMFLOAT3(1.0f, 0.72f, 0.42f);
            lightIntensity = 0.85f;
            ambientColor = XMFLOAT3(0.55f, 0.48f, 0.42f);
            ambientIntensity = 0.45f;
        }
        else
        {
            auto& tod = Spark::TimeOfDaySystem::GetInstance();
            const XMFLOAT3 sun = tod.GetSunDirection(); // unit vector TOWARD the sun (z == 0 arc)
            const float sunI = tod.GetSunIntensity();   // 0 below horizon .. 1 full day

            // Sun-vs-moon crossfade over the first ~6 elevation degrees so the
            // light never flips direction discontinuously at the horizon.
            const float w = std::clamp(sun.y / 0.1f, 0.0f, 1.0f);

            // Light direction: opposite the sun, tilted into +z so vertical
            // faces keep modelling (the old fixed baseline had the same tilt).
            const XMFLOAT3 sunLight = Norm3(-sun.x, -sun.y, 0.45f);
            constexpr XMFLOAT3 kMoonLight{0.356f, -0.814f, 0.458f}; // old baseline dir, normalized
            const XMFLOAT3 blended = Lerp3(kMoonLight, sunLight, w);
            lightDir = Norm3(blended.x, blended.y, blended.z);

            // Color: engine curve during the day (warm horizon -> white noon),
            // cool moonlight at night.
            constexpr XMFLOAT3 kMoonColor{0.45f, 0.50f, 0.65f};
            lightColor = Lerp3(kMoonColor, tod.GetSunColor(), w);

            // PS2 rule: a faint directional survives the night so shapes read.
            lightIntensity = std::max(kTFMoonIntensity, sunI);

            // Ambient: noon reproduces the old fixed baseline (0.6 warm bounce),
            // night floors at kTFNightAmbient with a cool sky tint.
            ambientIntensity = kTFNightAmbient + (kTFDayAmbient - kTFNightAmbient) * sunI;
            constexpr XMFLOAT3 kNightAmbColor{0.30f, 0.34f, 0.46f};
            constexpr XMFLOAT3 kDayAmbColor{0.52f, 0.50f, 0.46f}; // old baseline warm sky/ground bounce
            ambientColor = Lerp3(kNightAmbColor, kDayAmbColor, sunI);
        }

        m_seamPresent = TrySetEnvLighting(gfx, lightDir, lightColor, lightIntensity, ambientColor, ambientIntensity);
        m_pushedDirIntensity = lightIntensity;
        m_pushedAmbient = ambientIntensity;
    }

    // ---------------------------------------------------------------------------
    // Wire
    // ---------------------------------------------------------------------------

    void TFDayNight::BroadcastTime()
    {
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
        {
            TF_TimeOfDay msg{};
            msg.dayFrac = m_dayFrac;
            msg.rate = kTFDayRatePerSec;
            for (const auto& [id, info] : nm.GetClients())
            {
                if (info.state == Spark::Net::ConnectionState::Connected)
                    SendWire(id, msg);
            }
        }
#endif
        // Listen host / standalone local player: visuals read the server clock
        // directly through GetView — no loopback mirror write needed.
    }

#ifdef ENABLE_NETWORKING

    bool TFDayNight::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFDayNight::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgTimeOfDay),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_TimeOfDay))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_TimeOfDay msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               msg.dayFrac = WrapFrac(msg.dayFrac);
                               msg.rate = std::clamp(msg.rate, 0.0f, 1.0f); // sanity: <= 1 day per second
                               m_mirror = msg;
                               m_mirrorValid = true;
                               ++m_syncsRx;
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] time-of-day mirror handler registered");
    }

    void TFDayNight::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgTimeOfDay),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFDayNight::SendWire(PlayerId target, const TF_TimeOfDay& msg)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage out;
        out.type = static_cast<Spark::Net::MessageType>(kTFMsgTimeOfDay);
        out.channel = Spark::Net::ChannelType::Reliable;
        out.payload.resize(sizeof(msg));
        std::memcpy(out.payload.data(), &msg, sizeof(msg));
        nm.SendToClient(target, out);
        ++m_syncsSent;
    }

    void TFDayNight::PollJoins()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;

        // Late-joiner burst: unlike alerts, EVERY fresh client needs the clock
        // immediately (the mirror default is only the boot time).
        for (const auto& [id, info] : nm.GetClients())
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            TF_TimeOfDay msg{};
            msg.dayFrac = m_dayFrac;
            msg.rate = kTFDayRatePerSec;
            SendWire(id, msg);
        }

        // Leaver sweep: recycled-PlayerId hygiene.
        std::erase_if(m_knownClients, [&nm](PlayerId id) { return !nm.GetClients().contains(id); });
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFDayNight::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("Day/Night (TFDayNight)"))
            return;
        TF_TimeOfDay v{};
        GetView(v);
        ImGui::Text("time=%s (%s)  dayFrac=%.4f  rate=%.6f/s", ClockString(v.dayFrac).c_str(), PeriodName(v.dayFrac),
                    static_cast<double>(v.dayFrac), static_cast<double>(v.rate));
        ImGui::Text("role=%s  mirrorValid=%d  sanctuaryOverride=%d",
                    (m_ctx && m_ctx->IsAuthority()) ? "authority" : "client", m_mirrorValid ? 1 : 0,
                    m_sanctuaryOverride ? 1 : 0);
        ImGui::Text("pushed: dirIntensity=%.2f ambient=%.2f  seam=%s", static_cast<double>(m_pushedDirIntensity),
                    static_cast<double>(m_pushedAmbient), m_seamPresent ? "wired" : "MISSING (visuals inert)");
        ImGui::Text("syncs sent=%u rx=%u bad=%u engineSnaps=%u", m_syncsSent, m_syncsRx, m_badPackets, m_engineSnaps);
        if (m_ctx && m_ctx->IsAuthority())
            ImGui::Text("next drift sync in %.0fs", static_cast<double>(kTFTimeSyncSec - m_syncAccum));
    }

#else

    void TFDayNight::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
