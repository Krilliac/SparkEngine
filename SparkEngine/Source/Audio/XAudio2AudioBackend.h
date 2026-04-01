/**
 * @file XAudio2AudioBackend.h
 * @brief IAudioBackend adapter wrapping the existing XAudio2-based AudioEngine
 * @author Spark Engine Team
 * @date 2026
 *
 * Adapts the legacy AudioEngine (Windows/XAudio2) to the cross-platform
 * IAudioBackend interface. This is a thin delegation layer -- all real
 * audio work is done by AudioEngine.
 */

#pragma once

#include "IAudioBackend.h"

#ifdef SPARK_PLATFORM_WINDOWS

class AudioEngine;

namespace Spark::Audio
{

    /**
     * @brief XAudio2 backend adapter for Windows
     *
     * Wraps the existing AudioEngine class so the engine can use it through
     * the cross-platform IAudioBackend interface.
     */
    class XAudio2AudioBackend final : public IAudioBackend
    {
      public:
        /**
         * @brief Construct with a reference to the existing AudioEngine
         * @param engine Non-owning pointer to the engine's AudioEngine instance
         */
        explicit XAudio2AudioBackend(AudioEngine* engine);
        ~XAudio2AudioBackend() override = default;

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
        AudioEngine* m_engine; ///< Non-owning reference to the XAudio2 engine
    };

} // namespace Spark::Audio

#endif // SPARK_PLATFORM_WINDOWS
