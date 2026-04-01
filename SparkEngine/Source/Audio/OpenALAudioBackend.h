/**
 * @file OpenALAudioBackend.h
 * @brief IAudioBackend adapter wrapping OpenALAudioEngine for Linux/macOS
 * @author Spark Engine Team
 * @date 2026
 *
 * Adapts the OpenALAudioEngine to the cross-platform IAudioBackend interface.
 * Compiled only on non-Windows platforms where OpenAL Soft is available.
 */

#pragma once

#include "IAudioBackend.h"
#include "../Core/Platform.h"

#if !defined(SPARK_PLATFORM_WINDOWS)

#include "OpenALAudioEngine.h"

#include <memory>

namespace Spark::Audio
{

    /**
     * @brief OpenAL Soft backend adapter for Linux/macOS
     *
     * Wraps the OpenALAudioEngine so the engine can use it through the
     * cross-platform IAudioBackend interface. Owns the OpenALAudioEngine
     * instance.
     */
    class OpenALAudioBackend final : public IAudioBackend
    {
      public:
        OpenALAudioBackend();
        ~OpenALAudioBackend() override;

        // Lifecycle
        bool Initialize(size_t maxSources) override;
        void Shutdown() override;
        void Update(float deltaTime) override;

        // Sound loading
        bool LoadSound(const std::string& name, const std::string& filepath) override;
        void UnloadSound(const std::string& name) override;

        // Playback
        uint32_t PlaySound(const std::string& name, float volume, float pitch, bool loop) override;
        uint32_t PlaySound3D(const std::string& name, float x, float y, float z, float volume, float pitch,
                             bool loop) override;
        void StopSound(uint32_t handle) override;
        void StopAllSounds() override;
        void PauseAllSounds() override;
        void ResumeAllSounds() override;

        // Volume
        void SetMasterVolume(float volume) override;
        void SetSFXVolume(float volume) override;
        void SetMusicVolume(float volume) override;
        float GetMasterVolume() const override;
        float GetSFXVolume() const override;
        float GetMusicVolume() const override;

        // 3D listener
        void SetListenerPosition(float x, float y, float z) override;
        void SetListenerOrientation(float fx, float fy, float fz, float ux, float uy, float uz) override;
        void SetListenerVelocity(float vx, float vy, float vz) override;
        void Set3DEnabled(bool enabled) override;

        // Queries
        bool IsAvailable() const override;

      private:
        std::unique_ptr<OpenALAudioEngine> m_engine;
    };

} // namespace Spark::Audio

#endif // !SPARK_PLATFORM_WINDOWS
