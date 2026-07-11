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
 *    loop on the continent. This REPLACES the old single wind loop started by
 *    TFWorldSetup::MaybeStartAmbientAudio() — the integrator removes that
 *    call (see the W8 wiring notes) or the wind bed doubles up.
 *  - Schedules occasional one-shots on the continent: wind_loop_02/03 gusts,
 *    plus combat_distant.wav whose frequency/volume are biased by a 0..1
 *    decaying "activity" heat (bumped by EvPlayerKilled / EvRegionContested /
 *    EvRegionCaptured on the bus and by TFWeaponSystem::RemoteFireHeat()).
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

        std::unordered_set<std::string> m_loaded; ///< LoadSound already issued for these paths
        std::mt19937 m_rng{0xA3B1Eu};
    };

} // namespace Terrafront
