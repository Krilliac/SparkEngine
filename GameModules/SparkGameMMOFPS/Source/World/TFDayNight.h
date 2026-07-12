/**
 * @file TFDayNight.h
 * @brief Continent day/night cycle — server-authoritative clock (1 game day ==
 *        40 real minutes), TF_TimeOfDay wire sync, and the client-side lighting
 *        drive (sun arc + dawn/day/dusk/night curves) for the basic render path.
 *
 * OWNERSHIP: time-of-day lane (W12) — this header + TFDayNight.cpp.
 *
 * Design (TFAlertSystem precedent: server clock + client mirror in one class;
 * wire ids + packed structs live HERE, not in the frozen TFNetProtocol.h):
 *  - CLOCK: dayFrac in [0,1) — 0.0 midnight, 0.5 noon. The authority role
 *    (listen host / dedicated / standalone) advances it in Update() at
 *    kTFDayRatePerSec (wall-clock bookkeeping, NEVER sim-facing: nothing here
 *    feeds movement or collision, so the determinism contract is untouched).
 *  - WIRE: reserved TFMsg block 0x5478-0x547B.
 *      0x5478 S->C TF_TimeOfDay (reliable; late-join burst via the
 *             NetworkManager::GetClients() diff poll, a full-fanout refresh
 *             every kTFTimeSyncSec for drift correction, and an immediate
 *             broadcast after `tf_time set`).
 *      0x5479-0x547B free.
 *    Clients advance the mirror locally at the wire rate between syncs and
 *    snap on correction. Handlers self-register when the socket connects
 *    (TFMedalSystem EnsureClientHandlers pattern) — no TFClientNet edits.
 *  - VISUALS (any role with a local player): the module-image
 *    Spark::TimeOfDaySystem provides the sun direction / sun color / ambient
 *    curves; TFDayNight keeps its 24 h clock locked to dayFrac and remaps the
 *    curve outputs onto the current basic-path baseline (noon reproduces the
 *    old fixed daylight constants) before pushing them through the
 *    GraphicsEngine::SetEnvironmentLighting seam. The seam is detected with a
 *    C++23 requires-expression, so this lane compiles and no-ops before the
 *    engine-side wiring lands (RenderDebugUI shows seam presence).
 *    NOTE: TimeOfDaySystem::GetInstance() is a per-image static — the module
 *    DLL drives its OWN copy purely as curve math; only the shared
 *    GraphicsEngine instance (reached via IEngineContext::GetGraphics())
 *    carries the result across the DLL boundary.
 *  - PS2 READABILITY RULE: night ambient never drops below kTFNightAmbient
 *    (0.25) and a faint cool "moon" directional (kTFMoonIntensity) survives,
 *    so gameplay stays readable after dark. The skybox needs no edit: the sky
 *    quads run the same ambient+N.L basic shader, so the tint dims with the
 *    frame constants automatically; emissive materials (capture banners,
 *    warpgate glow strips) are an UNLIT additive term in the embedded PS and
 *    become navigation beacons at night for free.
 *  - SANCTUARY: fixed golden-hour preset (the ~17:30 curve, warmed) whenever
 *    the CAMERA is inside the sanctuary rectangle — the exact zone rule
 *    TFWorldSetup::DrawSky uses for the per-zone sky, so sky and lighting
 *    always agree.
 *  - CONSOLE: tf_time [set <frac|HH:MM>] — get/set for testing; set is
 *    authority-only and re-broadcasts immediately.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstdint>
#include <string>
#include <unordered_set>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5478-0x547B (time-of-day lane).
    // Rides NetworkManager's handler registry cast to Spark::Net::MessageType at
    // the call site (TFRepProtocol.h / TFFireFxProtocol.h precedent).
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgTimeOfDay = 0x5478; // S->C TF_TimeOfDay (reliable sync + join burst)
    // 0x5479-0x547B reserved for future time-of-day traffic.

#pragma pack(push, 1)

    /// S->C: the authoritative continent clock. One message carries everything a
    /// client needs to free-run between syncs: the current day fraction and the
    /// rate it advances at (day-fractions per real second; 0 == frozen clock).
    struct TF_TimeOfDay
    {
        float dayFrac; // [0,1): 0.0 midnight, 0.25 06:00, 0.5 noon, 0.75 18:00
        float rate;    // day-fractions per real second (kTFDayRatePerSec normally)
    };
    static_assert(sizeof(TF_TimeOfDay) == 8, "wire layout frozen");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Tuning
    // ---------------------------------------------------------------------------

    constexpr float kTFDayLengthSec = 40.0f * 60.0f;           ///< 1 game day == 40 real minutes
    constexpr float kTFDayRatePerSec = 1.0f / kTFDayLengthSec; ///< dayFrac advance per real second
    constexpr float kTFTimeSyncSec = 30.0f;                    ///< full-fanout drift-correction cadence
    constexpr float kTFDayStartFrac = 10.0f / 24.0f;           ///< server boot time (10:00 — mid-morning)
    constexpr float kTFNightAmbient = 0.25f;                   ///< PS2 rule: readable-night ambient floor
    constexpr float kTFDayAmbient = 0.6f;                      ///< noon ambient == old fixed baseline
    constexpr float kTFMoonIntensity = 0.15f;                  ///< faint cool directional after dark
    constexpr float kTFTodSnapHours = 0.05f;                   ///< engine-clock correction threshold (3 game-min)
    constexpr float kTFSanctuaryHour = 17.5f;                  ///< sanctuary's frozen golden hour

    class TFDayNight
    {
      public:
        TFDayNight();
        ~TFDayNight();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- cross-system / console surface ------------------------------------

        /// Unified clock view: server truth on authority roles, TF_TimeOfDay
        /// mirror on pure clients (the TFAlertSystem GetView split).
        void GetView(TF_TimeOfDay& out) const;

        /// Convenience: current day fraction from GetView.
        float DayFrac() const;

        /// True when the sun is below the horizon for the current view.
        bool IsNight() const;

        /// Authority: jump the continent clock (dayFrac wrapped into [0,1)) and
        /// re-broadcast immediately. False off-authority.
        bool ServerSetDayFrac(float dayFrac);

        /// tf_time status payload (any role).
        std::string StatusString() const;

        /// "HH:MM" for a day fraction.
        static std::string ClockString(float dayFrac);

      private:
        static float WrapFrac(float f);
        void DriveEngineClock(const TF_TimeOfDay& view, float dt); ///< lock Spark::TimeOfDaySystem to dayFrac
        void ApplyVisuals();                                       ///< curves -> basic-path frame lighting
        void BroadcastTime();                                      ///< reliable to every connected client

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
        void SendWire(PlayerId target, const TF_TimeOfDay& msg);
        void PollJoins(); ///< late-joiner burst (every joiner needs the clock)
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        bool m_consoleCmd{false}; ///< tf_time registered by this instance

        // --- server clock (authority only) ---
        float m_dayFrac{kTFDayStartFrac};
        float m_syncAccum{0.0f};

        // --- client mirror (pure clients) ---
        TF_TimeOfDay m_mirror{};
        bool m_mirrorValid{false};
        bool m_clientHandlers{false};

#ifdef ENABLE_NETWORKING
        // Late-joiner burst bookkeeping (TFAlertSystem PollJoins precedent).
        std::unordered_set<PlayerId> m_knownClients;
#endif

        // --- pushed lighting snapshot (debug UI) ---
        bool m_seamPresent{false}; ///< SetEnvironmentLighting found at compile time AND pushed this frame
        bool m_sanctuaryOverride{false};
        float m_pushedDirIntensity{0.0f};
        float m_pushedAmbient{0.0f};

        // debug counters
        uint32_t m_syncsSent{0};
        uint32_t m_syncsRx{0};
        uint32_t m_badPackets{0};
        uint32_t m_engineSnaps{0}; ///< SetTimeOfDay corrections applied
    };

} // namespace Terrafront
