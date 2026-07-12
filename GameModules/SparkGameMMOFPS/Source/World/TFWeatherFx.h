/**
 * @file TFWeatherFx.h
 * @brief Cindral dust-storm cycle: server-owned weather phase machine + pure
 *        client presentation (dust tint, wind-blown particles, cull fog-in,
 *        storm wind audio seams). W12 weather-visuals lane.
 *
 * OWNERSHIP: weather-visuals lane (W12) — this header + TFWeatherFx.cpp, plus
 * the small clearly-sectioned hooks in Game/TFAudioAmbience.* (storm wind bed)
 * and World/TFRegionDecor.cpp (storm cull scale). Wiring into the contended
 * TFWorldSetup.cpp (Update tick + Render call + skybox dim) goes through the
 * lane wiring notes.
 *
 * ## Why the module owns the cycle (and not Spark::WeatherSystem)
 * The engine's WeatherSystem (Graphics/WeatherSystem.h) is a purely LOCAL
 * simulation: it never replicates, and its outputs (FogSystem density,
 * ambient/directional multipliers, sky tint) are consumed by the engine's own
 * render pipeline — NOT by the module's device-direct basic-shader path
 * (TFWorldSetup::RenderWorld), which has no fog and reads its sky tint from
 * presentation.json. Driving it would give every machine a different,
 * invisible storm. So the cycle lives here, server-authoritative, mirrored to
 * pure clients with one tiny S->C message, and the visuals are honest
 * basic-path approximations:
 *  - screen-space dust tint: one camera-locked alpha quad sized from the
 *    projection matrix (subtle sandy wash, alpha scales with intensity);
 *  - wind-blown dust particles: the TFGroundFx flipbook billboard recipe
 *    (hover_dust.png strip, additive, depth read-only), horizontal
 *    wind-driven velocity, hard cap kStormMaxFlakes;
 *  - fog-in of distant objects: TFRegionDecor's cull pass multiplies its
 *    ranges by DecorCullScale() (1.0 clear -> kStormCullScale in a full
 *    storm), and TFWorldSetup::DrawSkybox multiplies its tint by SkyboxDim();
 *  - audio: TFAudioAmbience fades a wind_loop_02 storm bed up with
 *    StormIntensity01() and biases its gust one-shots.
 *
 * GAMEPLAY: NONE this wave — no accuracy/visibility/movement mechanics.
 * Nothing here touches the deterministic movement/collision surface; a pure
 * client with a dropped heartbeat just sees the storm a beat late.
 *
 * ## Cycle (authority roles only advance it)
 *   Clear (10-20 min) -> Building (60 s) -> Storm (3-5 min) -> Clearing (45 s)
 * Intensity envelope: Clear 0, Building t/dur, Storm 1, Clearing 1-t/dur;
 * clients smooth toward the envelope (~4 s full swing) so heartbeat snaps and
 * phase edges never pop.
 *
 * ## Sync (0x547C, reserved weather block 0x547C-0x547F)
 * Id + packed struct live HERE, deliberately OUTSIDE the frozen TFMsg enum —
 * the exact precedent of TFRepProtocol.h (0x54F0) / TFFireFxProtocol.h
 * (0x54F4). S->C only: TF_WeatherState heartbeat every kWeatherSyncSec
 * (unreliable) + one reliable send on every phase transition. Pure clients
 * self-register the handler via the TFSocialSystem poll pattern (no-op
 * replacement on release so no dangling `this` survives the module DLL).
 *
 * Singleton for the same reason as TFGroundFx: single producer per concern,
 * one-line wiring, no TFGameContext (frozen header) or Main.cpp changes.
 */
#pragma once

#include "Core/TFTypes.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs on Linux
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

class GraphicsEngine;
class Mesh;

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol (weather block 0x547C-0x547F; 0x547D-0x547F free)
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgWeatherState = 0x547C; // S->C TF_WeatherState

    /// Heartbeat period for the unreliable state broadcast (late joiners and
    /// dropped packets converge within one beat; transitions also send reliable).
    constexpr double kWeatherSyncSec = 2.0;

#pragma pack(push, 1)

    /// S->C: full weather phase state. Clients recompute the intensity envelope
    /// locally from phase + elapsed/duration and extrapolate between beats;
    /// intensityQ is carried for debug display and forward compatibility.
    struct TF_WeatherState
    {
        uint8_t phase;       // TFWeatherFx::Phase
        uint8_t intensityQ;  // server raw envelope * 255
        uint16_t elapsedDs;  // seconds-into-phase * 10 (deciseconds; 20 min = 12000)
        uint16_t durationDs; // phase duration * 10 (deciseconds)
    };
    static_assert(sizeof(TF_WeatherState) == 6, "wire layout frozen (weather heartbeat)");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // TFWeatherFx
    // ---------------------------------------------------------------------------

    class TFWeatherFx
    {
      public:
        enum class Phase : uint8_t
        {
            Clear = 0,
            Building,
            Storm,
            Clearing,
            COUNT
        };

        /// Process-wide instance (see the singleton note in the file header).
        static TFWeatherFx& Get();

        /// Advance the weather machine. Call once per frame on EVERY role
        /// (wired from TFWorldSetup::Update): authority roles run the cycle and
        /// broadcast 0x547C; pure clients poll-register the handler and
        /// extrapolate the mirrored phase. Cheap when nothing is happening.
        void Update(TFGameContext& ctx, float deltaTime);

        /// Draw the storm visuals for this frame (wind-blown dust billboards +
        /// the fullscreen dust tint). Call from TFWorldSetup::RenderWorld after
        /// the transparent pass, BeginFrame active. No-op while clear, headless,
        /// or on roles without a local player. Restores blend/depth/texture.
        void Render(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                    const DirectX::XMMATRIX& proj);

        /// Smoothed storm intensity [0, 1] — THE value every consumer reads
        /// (tint/particle density here, storm bed in TFAudioAmbience, cull
        /// scale + skybox dim below). 0 while clear.
        float StormIntensity01() const { return m_intensity; }

        /// Decor cull-range multiplier for TFRegionDecor::UpdateCulling —
        /// 1.0 clear, kStormCullScale in a full storm (fog-in approximation).
        float DecorCullScale() const;

        /// Skybox tint multiplier for TFWorldSetup::DrawSkybox — 1.0 clear,
        /// kStormSkyDim in a full storm.
        float SkyboxDim() const;

        Phase CurrentPhase() const { return m_phase; }
        static const char* PhaseName(Phase p);

      private:
        TFWeatherFx() = default;

        // --- cycle (authority) ---
        void AdvanceCycle(float dt);
        void EnterPhase(Phase p);
        float RollDurationSec(Phase p);
        /// Raw (unsmoothed) intensity from the current phase envelope.
        float PhaseEnvelope() const;

        // --- console ---
        void EnsureConsoleCommand();

        // --- net (both directions live in the .cpp under ENABLE_NETWORKING) ---
        void ServerMaybeBroadcast(bool transition);
        void ClientPollHandler(TFGameContext& ctx);
        void OnNetWeatherState(const void* data, size_t size);

        // --- render helpers ---
        void UpdateAndDrawFlakes(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                 const DirectX::XMMATRIX& proj, float dt);
        void DrawScreenTint(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);
        float Rand01();

        /// One wind-blown dust sprite (world space, TFGroundFx recipe).
        struct DustFlake
        {
            float pos[3] = {0.0f, 0.0f, 0.0f};
            float vel[3] = {0.0f, 0.0f, 0.0f};
            float age = 0.0f;
            float life = 2.0f;
            float size = 1.6f;
        };

        TFGameContext* m_ctx{nullptr}; // set each Update (console command reads it)

        // Phase machine (authority-advanced; client-mirrored via 0x547C).
        Phase m_phase{Phase::Clear};
        float m_elapsedSec{0.0f};
        float m_durationSec{900.0f};
        float m_intensity{0.0f}; ///< smoothed toward PhaseEnvelope()

        // Server broadcast pacing.
        double m_netClock{0.0};
        double m_nextBeat{0.0};

        bool m_netHandler{false}; ///< 0x547C client handler registered
        bool m_debugCmd{false};   ///< tf_weather registered by this instance

        // Storm particles + tint (client presentation only).
        std::vector<DustFlake> m_flakes;
        std::unique_ptr<Mesh> m_quad; ///< unit plane, billboarded per flake / tint (lazy)
        double m_lastRealTime{-1.0};  ///< render clock; -1 == first frame
        float m_windAngle{0.7f};      ///< world-space wind heading (radians, drifts slowly)

        std::minstd_rand m_rng{0xD5F7u};
    };

} // namespace Terrafront
