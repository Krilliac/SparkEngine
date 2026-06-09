/**
 * @file OpenALAudioEngine.h
 * @brief OpenAL Soft audio backend for Linux/macOS cross-platform audio
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a cross-platform audio backend using OpenAL Soft, matching the
 * AudioEngine interface. Used on Linux/macOS where XAudio2 is unavailable.
 * Supports 3D spatial audio, distance attenuation, Doppler effects, and
 * audio source pooling.
 *
 * Build: Requires OpenAL Soft (-lopenal) and is only compiled when
 *        SPARK_PLATFORM_LINUX or SPARK_PLATFORM_MACOS is defined.
 */

#pragma once
#include "../Core/Platform.h"

#if !defined(SPARK_PLATFORM_WINDOWS)

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declare OpenAL types to avoid header dependency in this header.
// The underlying struct tag differs by implementation: Apple's OpenAL.framework
// uses `struct ALCdevice_struct`, while OpenAL Soft (Linux/Windows) uses
// `struct ALCdevice`. The forward declaration must match the platform's real
// header or it is a "typedef redefinition with different types" error.
#ifdef __APPLE__
typedef struct ALCdevice_struct ALCdevice;
typedef struct ALCcontext_struct ALCcontext;
#else
typedef struct ALCdevice ALCdevice;
typedef struct ALCcontext ALCcontext;
#endif

namespace Spark::Audio
{

    struct Float3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /**
     * @brief Audio source for the OpenAL backend
     */
    struct OpenALSource
    {
        uint32_t alSource = 0;  ///< OpenAL source handle
        uint32_t alBuffer = 0;  ///< OpenAL buffer handle (shared with SoundBuffer)
        Float3 Position;        ///< 3D world position
        Float3 Velocity;        ///< 3D velocity for Doppler
        float Volume = 1.0f;    ///< Volume level
        float Pitch = 1.0f;     ///< Pitch multiplier
        bool Is3D = false;      ///< Whether source uses 3D positioning
        bool IsLooping = false; ///< Whether sound loops
        bool IsPlaying = false; ///< Whether currently playing
        uint32_t SourceID = 0;  ///< Unique ID for console tracking
        std::string SoundName;  ///< Name of the sound being played
    };

    /**
     * @brief In-memory sound buffer for OpenAL
     */
    struct SoundBuffer
    {
        uint32_t alBuffer = 0;      ///< OpenAL buffer handle
        uint32_t sampleRate = 0;    ///< Sample rate in Hz
        uint16_t channels = 0;      ///< Number of channels
        uint16_t bitsPerSample = 0; ///< Bits per sample
        float duration = 0.0f;      ///< Duration in seconds
    };

    /**
     * @brief Audio metrics for console integration
     */
    struct AudioMetrics
    {
        size_t activeSources = 0;
        size_t totalSources = 0;
        size_t loadedSounds = 0;
        float masterVolume = 1.0f;
        float sfxVolume = 1.0f;
        float musicVolume = 1.0f;
        bool is3DEnabled = true;
        Float3 listenerPosition;
        float dopplerScale = 1.0f;
        float distanceScale = 1.0f;
    };

    /**
     * @brief OpenAL Soft audio engine for Linux/macOS
     *
     * Drop-in replacement for the XAudio2 AudioEngine on non-Windows platforms.
     * Provides the same feature set: 2D/3D audio, source pooling, volume control,
     * Doppler effects, and console integration.
     */
    class OpenALAudioEngine
    {
      public:
        OpenALAudioEngine();
        ~OpenALAudioEngine();

        /// @brief Initialize OpenAL with the specified number of source voices
        /// @return true on success, false on failure
        bool Initialize(size_t maxSources = 32);

        /// @brief Update audio state (call once per frame)
        void Update(float deltaTime);

        /// @brief Shut down OpenAL and release all resources
        void Shutdown();

        /// @brief Load a WAV sound effect from file
        bool LoadSound(const std::string& name, const std::string& filename);

        /// @brief Unload a previously loaded sound effect
        void UnloadSound(const std::string& name);

        /// @brief Play a 2D sound effect
        OpenALSource* PlaySound(const std::string& name, float volume = 1.0f, float pitch = 1.0f, bool loop = false);

        /// @brief Play a 3D positioned sound effect
        OpenALSource* PlaySound3D(const std::string& name, const Float3& position, float volume = 1.0f,
                                  float pitch = 1.0f, bool loop = false);

        /// @brief Stop a specific audio source
        void StopSound(OpenALSource* source);

        /// @brief Stop all playing sounds
        void StopAllSounds();

        /// @brief Pause all playing sounds
        void PauseAllSounds();

        /// @brief Resume all paused sounds
        void ResumeAllSounds();

        // Volume controls
        void SetMasterVolume(float volume);
        void SetSFXVolume(float volume);
        void SetMusicVolume(float volume);
        float GetMasterVolume() const { return m_masterVolume; }
        float GetSFXVolume() const { return m_sfxVolume; }
        float GetMusicVolume() const { return m_musicVolume; }

        // 3D Audio
        void SetListenerPosition(const Float3& pos);
        void SetListenerOrientation(const Float3& forward, const Float3& up);
        void SetListenerVelocity(const Float3& vel);
        void SetDopplerScale(float scale);
        void SetDistanceScale(float scale);
        void Set3DEnabled(bool enabled);

        // Queries
        size_t GetActiveSourceCount() const;
        bool IsInitialized() const { return m_initialized; }
        AudioMetrics GetMetrics() const;

        // Console integration
        std::string Console_ListSounds() const;
        std::string Console_GetStatus() const;

      private:
        /// @brief Load a WAV file and create an OpenAL buffer
        bool LoadWAVFile(const std::string& filename, SoundBuffer& outBuffer);

        /// @brief Get an available source from the pool
        OpenALSource* GetAvailableSource();

        /// @brief Return a source to the pool
        void ReturnSource(OpenALSource* source);

        /// @brief Update source states (check for finished playback)
        void UpdateSources();

        ALCdevice* m_device = nullptr;
        ALCcontext* m_context = nullptr;
        bool m_initialized = false;

        float m_masterVolume = 1.0f;
        float m_sfxVolume = 1.0f;
        float m_musicVolume = 1.0f;
        float m_dopplerScale = 1.0f;
        float m_distanceScale = 1.0f;
        bool m_3DEnabled = true;

        size_t m_maxSources = 32;
        uint32_t m_nextSourceID = 1;

        std::vector<std::unique_ptr<OpenALSource>> m_sources;
        std::vector<OpenALSource*> m_availableSources;
        std::unordered_map<std::string, SoundBuffer> m_sounds;

        mutable std::mutex m_mutex;
    };

} // namespace Spark::Audio

#endif // !SPARK_PLATFORM_WINDOWS
