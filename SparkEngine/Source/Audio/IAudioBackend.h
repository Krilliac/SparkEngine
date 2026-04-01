/**
 * @file IAudioBackend.h
 * @brief Abstract audio backend interface for cross-platform audio support
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines a platform-agnostic interface that all audio backends must implement.
 * Concrete implementations include XAudio2 (Windows), OpenAL Soft (Linux/macOS),
 * and NullAudioBackend (headless / unsupported platforms). The interface uses
 * plain float parameters for positions to avoid DirectX type dependencies.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace Spark::Audio
{

    /**
     * @brief Abstract interface for audio backend implementations
     *
     * All audio playback in SparkEngine goes through this interface.
     * Backends are selected at startup based on platform and availability.
     * The engine always has a valid IAudioBackend -- at minimum, NullAudioBackend.
     */
    class IAudioBackend
    {
      public:
        virtual ~IAudioBackend() = default;

        // ====================================================================
        // Lifecycle
        // ====================================================================

        /**
         * @brief Initialize the audio backend
         * @param maxSources Maximum number of simultaneous audio sources
         * @return true on success, false on failure
         */
        virtual bool Initialize(size_t maxSources) = 0;

        /**
         * @brief Shut down the backend and release all resources
         */
        virtual void Shutdown() = 0;

        /**
         * @brief Per-frame update (3D recalculation, source recycling, etc.)
         * @param deltaTime Seconds elapsed since the previous frame
         */
        virtual void Update(float deltaTime) = 0;

        // ====================================================================
        // Sound loading
        // ====================================================================

        /**
         * @brief Load a sound effect from a file
         * @param name Unique identifier for the sound
         * @param filepath Path to the audio file (WAV, OGG, etc.)
         * @return true on success, false on failure
         */
        virtual bool LoadSound(const std::string& name, const std::string& filepath) = 0;

        /**
         * @brief Unload a previously loaded sound effect
         * @param name Identifier of the sound to unload
         */
        virtual void UnloadSound(const std::string& name) = 0;

        // ====================================================================
        // Playback
        // ====================================================================

        /**
         * @brief Play a 2D (non-positional) sound
         * @param name Identifier of the loaded sound
         * @param volume Volume multiplier (0.0 = silent, 1.0 = full)
         * @param pitch Pitch multiplier (1.0 = normal)
         * @param loop true to loop continuously
         * @return Opaque handle for the playing instance, or 0 on failure
         */
        virtual uint32_t PlaySound(const std::string& name, float volume = 1.0f, float pitch = 1.0f,
                                   bool loop = false) = 0;

        /**
         * @brief Play a 3D positional sound
         * @param name Identifier of the loaded sound
         * @param x World-space X position
         * @param y World-space Y position
         * @param z World-space Z position
         * @param volume Base volume before distance attenuation
         * @param pitch Pitch multiplier (1.0 = normal)
         * @param loop true to loop continuously
         * @return Opaque handle for the playing instance, or 0 on failure
         */
        virtual uint32_t PlaySound3D(const std::string& name, float x, float y, float z, float volume = 1.0f,
                                     float pitch = 1.0f, bool loop = false) = 0;

        /**
         * @brief Stop a specific sound instance
         * @param handle Handle returned by PlaySound / PlaySound3D
         */
        virtual void StopSound(uint32_t handle) = 0;

        /** @brief Stop all currently playing sounds */
        virtual void StopAllSounds() = 0;

        /** @brief Pause all currently playing sounds */
        virtual void PauseAllSounds() = 0;

        /** @brief Resume all previously paused sounds */
        virtual void ResumeAllSounds() = 0;

        // ====================================================================
        // Volume
        // ====================================================================

        /** @brief Set master volume (0.0 - 1.0) */
        virtual void SetMasterVolume(float volume) = 0;

        /** @brief Set sound-effects volume (0.0 - 1.0) */
        virtual void SetSFXVolume(float volume) = 0;

        /** @brief Set music volume (0.0 - 1.0) */
        virtual void SetMusicVolume(float volume) = 0;

        /** @return Current master volume */
        virtual float GetMasterVolume() const = 0;

        /** @return Current sound-effects volume */
        virtual float GetSFXVolume() const = 0;

        /** @return Current music volume */
        virtual float GetMusicVolume() const = 0;

        // ====================================================================
        // 3D listener
        // ====================================================================

        /**
         * @brief Set the listener world-space position
         * @param x X coordinate
         * @param y Y coordinate
         * @param z Z coordinate
         */
        virtual void SetListenerPosition(float x, float y, float z) = 0;

        /**
         * @brief Set the listener orientation
         * @param forwardX Forward direction X
         * @param forwardY Forward direction Y
         * @param forwardZ Forward direction Z
         * @param upX Up direction X
         * @param upY Up direction Y
         * @param upZ Up direction Z
         */
        virtual void SetListenerOrientation(float forwardX, float forwardY, float forwardZ, float upX, float upY,
                                            float upZ) = 0;

        /**
         * @brief Set the listener velocity (for Doppler calculations)
         * @param vx Velocity X
         * @param vy Velocity Y
         * @param vz Velocity Z
         */
        virtual void SetListenerVelocity(float vx, float vy, float vz) = 0;

        /**
         * @brief Enable or disable 3D audio processing
         * @param enabled true to enable, false to disable
         */
        virtual void Set3DEnabled(bool enabled) = 0;

        // ====================================================================
        // Queries
        // ====================================================================

        /**
         * @brief Check whether this backend is functional
         * @return true if the backend initialized successfully and can play audio
         */
        virtual bool IsAvailable() const = 0;

      protected:
        IAudioBackend() = default;

        // Non-copyable, non-movable
        IAudioBackend(const IAudioBackend&) = delete;
        IAudioBackend& operator=(const IAudioBackend&) = delete;
        IAudioBackend(IAudioBackend&&) = delete;
        IAudioBackend& operator=(IAudioBackend&&) = delete;
    };

} // namespace Spark::Audio
