/**
 * @file IAudioBackend.h
 * @brief Abstract audio backend interface for swappable audio engines
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines the interface that audio implementations (XAudio2, OpenAL, etc.)
 * must implement. This abstraction enables platform-specific backends to be
 * swapped without modifying engine code that uses audio.
 *
 * Currently, AudioEngine implements this interface using XAudio2 on Windows.
 * A future OpenAL or SDL_mixer backend would implement the same interface.
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Spark::Audio
{

    /**
     * @brief Abstract interface for audio engine backends.
     *
     * All audio operations go through this interface. Implementations manage
     * their own internal state (audio device, voices, buffers) and translate
     * between the engine's types and the backend's native API.
     */
    class IAudioBackend
    {
      public:
        virtual ~IAudioBackend() = default;

        // ================================================================
        // Lifecycle
        // ================================================================

        /// @brief Initialize the audio backend with the given max simultaneous sources.
        /// @param maxSources Maximum number of simultaneous audio sources.
        /// @return true on success.
        virtual bool Initialize(size_t maxSources) = 0;

        /// @brief Shut down and release all audio resources.
        virtual void Shutdown() = 0;

        /// @brief Update the audio system (call once per frame).
        /// @param deltaTime Time elapsed since last frame in seconds.
        virtual void Update(float deltaTime) = 0;

        // ================================================================
        // Playback
        // ================================================================

        /// @brief Play a 2D sound effect.
        /// @param name Name of the loaded sound.
        /// @param volume Volume (0.0-1.0+).
        /// @param pitch Pitch multiplier.
        /// @param loop Whether to loop.
        /// @return Opaque source ID, or 0 on failure.
        virtual uint32_t PlaySound(const std::string& name, float volume = 1.0f, float pitch = 1.0f,
                                   bool loop = false) = 0;

        /// @brief Play a 3D positioned sound effect.
        /// @param name Name of the loaded sound.
        /// @param position 3D position where the sound originates.
        /// @param volume Base volume before 3D attenuation.
        /// @param pitch Pitch multiplier.
        /// @param loop Whether to loop.
        /// @return Opaque source ID, or 0 on failure.
        virtual uint32_t PlaySound3D(const std::string& name, const DirectX::XMFLOAT3& position, float volume = 1.0f,
                                     float pitch = 1.0f, bool loop = false) = 0;

        /// @brief Stop a specific audio source by ID.
        virtual void StopSound(uint32_t sourceID) = 0;

        /// @brief Stop all currently playing sounds.
        virtual void StopAllSounds() = 0;

        /// @brief Pause all currently playing sounds.
        virtual void PauseAllSounds() = 0;

        /// @brief Resume all paused sounds.
        virtual void ResumeAllSounds() = 0;

        // ================================================================
        // Sound Management
        // ================================================================

        /// @brief Load a sound from file.
        /// @param name Unique name to identify the sound.
        /// @param filename Path to the audio file.
        /// @return true on success.
        virtual bool LoadSound(const std::string& name, const std::wstring& filename) = 0;

        /// @brief Unload a previously loaded sound.
        virtual void UnloadSound(const std::string& name) = 0;

        // ================================================================
        // Volume Controls
        // ================================================================

        virtual void SetMasterVolume(float volume) = 0;
        virtual void SetSFXVolume(float volume) = 0;
        virtual void SetMusicVolume(float volume) = 0;

        virtual float GetMasterVolume() const = 0;
        virtual float GetSFXVolume() const = 0;
        virtual float GetMusicVolume() const = 0;

        // ================================================================
        // 3D Audio
        // ================================================================

        /// @brief Set the listener position for 3D audio.
        virtual void SetListenerPosition(const DirectX::XMFLOAT3& position) = 0;

        /// @brief Set the listener orientation for 3D audio.
        virtual void SetListenerOrientation(const DirectX::XMFLOAT3& forward, const DirectX::XMFLOAT3& up) = 0;

        // ================================================================
        // Query
        // ================================================================

        /// @brief Get the number of currently active audio sources.
        virtual size_t GetActiveSourceCount() const = 0;

        /// @brief Check whether a real audio backend is available on this platform.
        virtual bool IsAvailable() const = 0;

        /// @brief Get the backend name (e.g., "XAudio2", "OpenAL").
        virtual const char* GetBackendName() const = 0;
    };

} // namespace Spark::Audio
