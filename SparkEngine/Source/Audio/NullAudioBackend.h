/**
 * @file NullAudioBackend.h
 * @brief No-op audio backend for headless and unsupported platforms
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements IAudioBackend with empty method bodies so the engine can run
 * without a real audio device.  Logs once at initialization, then silently
 * discards every request.  Guarantees the engine always has a valid backend.
 */

#pragma once

#include "IAudioBackend.h"

#include <cstdint>
#include <string>

namespace Spark::Audio
{

    /**
     * @brief Silent audio backend -- every method is a no-op
     *
     * Used on platforms without a supported audio API, in headless server
     * builds, and as a safe fallback when the preferred backend fails to
     * initialize.
     */
    class NullAudioBackend final : public IAudioBackend
    {
      public:
        NullAudioBackend() = default;
        ~NullAudioBackend() override = default;

        // ====================================================================
        // Lifecycle
        // ====================================================================

        /** @brief Logs initialization and returns true (always succeeds) */
        bool Initialize(size_t /*maxSources*/) override
        {
            m_initialized = true;
            // Intentionally no LOG_INFO here -- callers log backend selection.
            return true;
        }

        void Shutdown() override { m_initialized = false; }

        void Update(float /*deltaTime*/) override {}

        // ====================================================================
        // Sound loading
        // ====================================================================

        bool LoadSound(const std::string& /*name*/, const std::string& /*filepath*/) override { return true; }

        void UnloadSound(const std::string& /*name*/) override {}

        // ====================================================================
        // Playback
        // ====================================================================

        uint32_t PlaySound(const std::string& /*name*/, float /*volume*/, float /*pitch*/, bool /*loop*/) override
        {
            return 0;
        }

        uint32_t PlaySound3D(const std::string& /*name*/, float /*x*/, float /*y*/, float /*z*/, float /*volume*/,
                             float /*pitch*/, bool /*loop*/) override
        {
            return 0;
        }

        void StopSound(uint32_t /*handle*/) override {}
        void StopAllSounds() override {}
        void PauseAllSounds() override {}
        void ResumeAllSounds() override {}

        // ====================================================================
        // Volume
        // ====================================================================

        void SetMasterVolume(float volume) override { m_masterVolume = volume; }
        void SetSFXVolume(float volume) override { m_sfxVolume = volume; }
        void SetMusicVolume(float volume) override { m_musicVolume = volume; }

        float GetMasterVolume() const override { return m_masterVolume; }
        float GetSFXVolume() const override { return m_sfxVolume; }
        float GetMusicVolume() const override { return m_musicVolume; }

        // ====================================================================
        // 3D listener
        // ====================================================================

        void SetListenerPosition(float /*x*/, float /*y*/, float /*z*/) override {}
        void SetListenerOrientation(float /*forwardX*/, float /*forwardY*/, float /*forwardZ*/, float /*upX*/,
                                    float /*upY*/, float /*upZ*/) override
        {
        }
        void SetListenerVelocity(float /*vx*/, float /*vy*/, float /*vz*/) override {}
        void Set3DEnabled(bool /*enabled*/) override {}

        // ====================================================================
        // Queries
        // ====================================================================

        /** @return Always false -- no real audio hardware */
        bool IsAvailable() const override { return false; }

      private:
        bool m_initialized = false;
        float m_masterVolume = 1.0f;
        float m_sfxVolume = 1.0f;
        float m_musicVolume = 1.0f;
    };

} // namespace Spark::Audio
