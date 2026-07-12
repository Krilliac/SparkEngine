/**
 * @file TFAudioAmbience.h
 * @brief Per-zone ambient audio beds + activity-biased one-shot layers.
 *
 * OWNERSHIP: audio-polish lane (W8). New system, uniform TF lifecycle
 * (Initialize/Update/FixedUpdate/Shutdown/RenderDebugUI), wired from Main.cpp
 * by the integrator. Client-only: a dedicated server (no local player) or a
 * headless run (GetAudio() null) leaves the whole system inert.
 *
 * ## What it does
 *  - Owns TWO looping ambient beds and crossfades them (~1.5 s) by camera
 *    zone: sanctuary_hum.wav inside the sanctuary rectangle
 *    (TFTravel_IsInSanctuary on the camera XZ), the presentation.json wind
 *    loop on the continent. This REPLACES the old single wind loop that
 *    TFWorldSetup::MaybeStartAmbientAudio() used to start (dead code removed
 *    in W9).
 *  - Schedules occasional one-shots on the continent: wind_loop_02/03 gusts,
 *    plus combat_distant.wav whose frequency/volume are biased by a 0..1
 *    decaying "activity" heat (bumped by EvPlayerKilled / EvRegionContested /
 *    EvRegionCaptured on the bus and by TFWeaponSystem::RemoteFireHeat()).
 *  - W9 remote-fire-events: on PURE clients, self-registers the 0x54F4
 *    TF_RemoteFireFx handler (Net/TFFireFxProtocol.h; TFSocialSystem's
 *    poll-registration pattern) and routes each message through
 *    ClientOnRemoteFire(): a world-space muzzle flash quad
 *    (TFWorldSetup::SpawnMuzzleFx) plus the W8 distance-bucketed remote-fire
 *    audio via TFWeaponSystem::ClientOnRemoteFire — which also bumps
 *    RemoteFireHeat(), so the existing heat merge in Update() feeds the
 *    combat layer with zero extra plumbing. Listen hosts never receive
 *    0x54F4 (the server excludes the shooter and only SENDS to remote
 *    clients); their path stays the in-process ServerHandleFire hook.
 *
 * ## AudioEngine usage contract (Audio/AudioEngine.h)
 * PlaySound(name, volume, pitch, loop) returns an AudioSource* from a reused
 * pool; per-source volume is only applied at play time, so the crossfade
 * writes src->Voice->SetVolume() directly each frame (mirroring PlaySound's
 * volume * sfx * master scaling). Pool slots can be stolen back by
 * StopAllSounds()/scene changes, so every held pointer is REVALIDATED each
 * frame (IsPlaying + IsLooping + Sound == GetSound(path)) before use and the
 * bed is lazily restarted when the check fails.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>

class AudioEngine;
struct AudioSource;

namespace Terrafront
{

    class TFAudioAmbience
    {
      public:
        TFAudioAmbience();
        ~TFAudioAmbience();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        /// W9 remote-fire-events: client fx seam for one remote validated shot
        /// (0x54F4 TF_RemoteFireFx, or any future client-side caller). Spawns a
        /// brief world-space flash quad at the muzzle and plays the W8
        /// distance-bucketed remote-fire audio by routing through
        /// TFWeaponSystem::ClientOnRemoteFire (near: weapon's own clip,
        /// far: faction distant_fire_* tail, capped) — which also bumps
        /// RemoteFireHeat(), feeding this system's combat heat via the
        /// existing merge in Update(). `dirUnit` is the fire direction
        /// (BuildViewRay convention); `shooterEntity` resolves faction/owner
        /// from the replicated pawn mirror (unknown entity -> common tail).
        void ClientOnRemoteFire(const float muzzlePos[3], const float dirUnit[3], WeaponId weaponId,
                                EntityId shooterEntity);

      private:
        /// One looping ambient bed (sanctuary hum / continent wind).
        struct Bed
        {
            std::string path;             ///< Assets-relative wav path (doubles as the sound-cache key)
            ::AudioSource* src = nullptr; ///< held looping source (revalidated every frame, see file header)
            float vol = 0.0f;             ///< current faded volume (pre master/sfx scaling)
        };

        ::AudioEngine* Audio() const;
        bool ListenerXZ(float& outX, float& outZ) const;
        void EnsureLoaded(::AudioEngine& audio, const std::string& assetPath);
        void UpdateBeds(::AudioEngine& audio, bool inSanctuary, float dt);
        void UpdateOneShots(::AudioEngine& audio, bool inSanctuary);
        void StopBeds();

        // W10 audio-wave-2: looping capture_alarm.wav while the LOCAL pawn
        // stands on a capture point that is actively being captured or
        // contested. Uses the same predicate family as the HUD capture feed
        // (TFRegionSystem::FeedLocalCaptureHUD "live" check) rebuilt from the
        // PUBLIC accessors (CaptureProgress + regions.json points + the 10 m
        // DESIGN §4 radius) so no contended file changes. Fast fade in/out via
        // the same held-source revalidation discipline as the beds.
        bool LocalOnActiveCapturePoint() const;
        void UpdateCaptureAlarm(::AudioEngine& audio, float dt);

        // W9 remote-fire-events: 0x54F4 handler lifecycle (TFSocialSystem
        // pattern — pure-client only, polled from Update, released with a
        // no-op handler on Shutdown so no dangling `this` survives the module
        // DLL). W11 impact-broadcast: the 0x54F5 TF_ImpactFx handler rides the
        // same lifecycle (inline lambda -> TFImpactFx::OnServerImpact).
        // Definitions live under ENABLE_NETWORKING in the .cpp.
        bool ClientNetActive() const;
        void EnsureNetHandlers();
        void ReleaseNetHandlers();
        void OnNetRemoteFireFx(const void* data, size_t size);

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        Bed m_sanctuaryBed;
        Bed m_windBed;

        double m_clock{0.0};
        /// 0..1 decaying combat heat: kills/captures/contests bump it, ~10 s
        /// half-life decay; biases combat_distant one-shot rate + volume.
        float m_activity{0.0f};
        double m_nextCombatShot{0.0};
        double m_nextGust{0.0};
        bool m_lastInSanctuary{false};
        Bed m_alarmBed;        ///< W10 audio-wave-2: capture-alarm loop (Bed reuse)
        bool m_alarmOn{false}; ///< W10 audio-wave-2: alarm predicate this frame
        // W12 weather-visuals: dust-storm wind loop (wind_loop_02) riding the
        // same Bed crossfade machinery; target volume follows
        // TFWeatherFx::Get().StormIntensity01() (0 while clear / in sanctuary).
        Bed m_stormBed;
        bool m_netHandlers{false}; ///< 0x54F4 + 0x54F5 client handlers registered (W9/W11)

        std::unordered_set<std::string> m_loaded; ///< LoadSound already issued for these paths
        std::mt19937 m_rng{0xA3B1Eu};
    };

} // namespace Terrafront
