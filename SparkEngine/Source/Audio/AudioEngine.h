/**
 * @file AudioEngine.h
 * @brief XAudio2-based audio engine with comprehensive console integration
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive audio system built on XAudio2, supporting
 * both 2D and 3D audio playback, sound effect management, volume controls, and
 * an object pool system for efficient audio source management. Enhanced with
 * full console integration for real-time audio debugging and parameter adjustment.
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#include "SoundEffect.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <xaudio2.h>
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <cstdint>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

namespace Spark::Audio
{
    class AudioMixer;
}

/**
 * @brief Volume category a playback request is billed to
 *
 * The category selects which stored volume -- and, when a mixer is attached,
 * which mix bus -- scales the caller's requested volume. Master volume is NOT
 * part of that chain: it is applied once, on the mastering voice.
 */
enum class AudioCategory : uint8_t
{
    SFX,  ///< Sound effects: scaled by the SFX volume and the "SFX" bus
    Music ///< Music: scaled by the music volume and the "Music" bus
};

/**
 * @brief Audio source structure for managing individual sound instances
 * 
 * Represents a single audio source that can play a sound effect with specific
 * properties like position, velocity, volume, and pitch. Used for both 2D
 * and 3D audio playback within the audio engine's object pool system.
 */
struct AudioSource
{
    IXAudio2SourceVoice* Voice; ///< XAudio2 source voice for audio playback
    XMFLOAT3 Position;          ///< 3D world position for spatial audio
    XMFLOAT3 Velocity;          ///< 3D velocity for Doppler effects
    float RequestedVolume;      ///< Caller's unscaled volume request (0.0 to 1.0)
    float Volume;               ///< Post-category gain actually sent to the voice
    float Pitch;                ///< Pitch multiplier (1.0 = normal pitch)
    float MinDistance;          ///< Distance at which 3D attenuation begins (meters)
    float MaxDistance;          ///< Distance at which a 3D source is fully attenuated (meters)
    bool Is3D;                  ///< Whether this source uses 3D positioning
    bool IsLooping;             ///< Whether the sound should loop continuously
    bool IsPlaying;             ///< Whether the source is currently playing
    bool HasVoiceFormat;        ///< Whether VoiceFormat describes the live source voice
    AudioCategory Category;     ///< Volume category this playback is billed to
    SoundEffect* Sound;         ///< Pointer to the associated sound effect
    uint32_t SourceID;          ///< Unique identifier for console tracking
    uint32_t Generation;        ///< Incremented on every acquire; stale handles compare unequal
    WAVEFORMATEX VoiceFormat;   ///< Format the live source voice was created with

    /**
     * @brief Default constructor with safe initial values
     * 
     * Initializes all members to safe defaults suitable for audio playback.
     */
    AudioSource()
        : Voice(nullptr), Position(0, 0, 0), Velocity(0, 0, 0), RequestedVolume(1.0f), Volume(1.0f), Pitch(1.0f),
          MinDistance(1.0f), MaxDistance(50.0f), Is3D(false), IsLooping(false), IsPlaying(false), HasVoiceFormat(false),
          Category(AudioCategory::SFX), Sound(nullptr), SourceID(0), Generation(0), VoiceFormat{}
    {
    }
};

/**
 * @brief Main audio engine class with comprehensive console integration
 * 
 * The AudioEngine class manages all audio operations for the Spark Engine using
 * XAudio2 as the underlying audio API. It provides sound loading, 3D spatial audio,
 * volume controls, efficient audio source management through object pooling, and
 * comprehensive console integration for real-time debugging and tuning.
 * 
 * Features include:
 * - XAudio2-based audio playback with hardware acceleration
 * - 2D and 3D spatial audio positioning
 * - Sound effect loading and management
 * - Volume controls (master, SFX, music)
 * - Audio source pooling for performance
 * - Looping and one-shot audio playback
 * - Pitch and volume controls per source
 * - Real-time console integration for debugging
 * - Live audio parameter adjustment
 * - Performance monitoring and analysis
 * 
 * @note The engine uses an object pool to efficiently reuse audio sources
 * @warning Initialize() must be called before any audio operations
 */
class AudioEngine
{
  public:
    /**
     * @brief Default constructor
     * 
     * Initializes member variables to safe default values. Call Initialize()
     * to set up the XAudio2 system.
     */
    AudioEngine();

    /**
     * @brief Destructor
     * 
     * Automatically calls Shutdown() to ensure proper cleanup of all
     * XAudio2 resources and audio sources.
     */
    ~AudioEngine();

    /**
     * @brief Check whether a real audio backend is available on this platform
     *
     * Returns true on Windows (XAudio2). Returns false on Linux/macOS where
     * audio stubs are no-ops. Wine wraps XAudio2 over ALSA/PulseAudio; a
     * native SDL2 audio backend is planned but not yet implemented.
     */
    static bool IsAudioBackendAvailable()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        return true;
#elif SPARK_HAS_MINIAUDIO
        return true; // miniaudio provides cross-platform audio
#else
        return false;
#endif
    }

    /**
     * @brief Initialize the audio engine with XAudio2
     *
     * Sets up the XAudio2 engine, creates the mastering voice, and initializes
     * the audio source object pool with the specified number of sources.
     *
     * @param maxSources Maximum number of simultaneous audio sources
     * @return HRESULT indicating success or failure of audio initialization
     * @note A typical value for maxSources is 32-64 for most games
     */
    HRESULT Initialize(size_t maxSources);

    /**
     * @brief Update the audio engine for the current frame
     * 
     * Updates all active audio sources, processes 3D audio calculations,
     * and manages the audio source pool. Should be called once per frame.
     * 
     * @param deltaTime Time elapsed since last frame in seconds
     */
    void Update(float deltaTime);

    /**
     * @brief Shutdown the audio engine and clean up resources
     * 
     * Stops all playing sounds, releases all XAudio2 resources, and cleans
     * up the audio source pool. Safe to call multiple times.
     */
    void Shutdown();

    /**
     * @brief Load a sound effect from file
     * 
     * Loads an audio file and associates it with a name for later playback.
     * Supports common audio formats like WAV files.
     * 
     * @param name Unique name to identify the sound effect
     * @param filename Path to the audio file to load
     * @return HRESULT indicating success or failure of sound loading
     */
    HRESULT LoadSound(const std::string& name, const std::wstring& filename);

    /**
     * @brief Unload a previously loaded sound effect
     * 
     * Removes a sound effect from memory and stops any instances currently
     * playing that sound.
     * 
     * @param name Name of the sound effect to unload
     */
    void UnloadSound(const std::string& name);

    /**
     * @brief Get a pointer to a loaded sound effect
     * 
     * Retrieves a sound effect by name for direct manipulation or inspection.
     * 
     * @param name Name of the sound effect to retrieve
     * @return Pointer to the SoundEffect, or nullptr if not found
     */
    SoundEffect* GetSound(const std::string& name);

    /**
     * @brief Play a 2D sound effect
     * 
     * Plays a sound effect without 3D positioning. The sound will be heard
     * at the same volume regardless of listener position.
     * 
     * @param name Name of the loaded sound effect to play
     * @param volume Volume level (0.0 to 1.0+, default: 1.0)
     * @param pitch Pitch multiplier (1.0 = normal pitch, default: 1.0)
     * @param loop Whether to loop the sound continuously (default: false)
     * @param category Volume category the request is billed to (default: SFX)
     * @return Pointer to the AudioSource playing the sound, or nullptr if failed
     */
    AudioSource* PlaySound(const std::string& name, float volume = 1.0f, float pitch = 1.0f, bool loop = false,
                           AudioCategory category = AudioCategory::SFX);

    /**
     * @brief Play a 3D positioned sound effect
     * 
     * Plays a sound effect with 3D spatial positioning. The volume and stereo
     * panning will be calculated based on the distance and direction from the listener.
     * 
     * @param name Name of the loaded sound effect to play
     * @param position 3D world position where the sound originates
     * @param volume Base volume level before 3D attenuation (default: 1.0)
     * @param pitch Pitch multiplier (1.0 = normal pitch, default: 1.0)
     * @param loop Whether to loop the sound continuously (default: false)
     * @param category Volume category the request is billed to (default: SFX)
     * @return Pointer to the AudioSource playing the sound, or nullptr if failed
     */
    AudioSource* PlaySound3D(const std::string& name, const XMFLOAT3& position, float volume = 1.0f, float pitch = 1.0f,
                             bool loop = false, AudioCategory category = AudioCategory::SFX);

    /**
     * @brief Stop a specific audio source
     * 
     * Stops playback of the specified audio source and returns it to the
     * available source pool for reuse.
     * 
     * @param source Pointer to the AudioSource to stop
     */
    void StopSound(AudioSource* source);

    /**
     * @brief Stop all currently playing sounds
     * 
     * Immediately stops all active audio sources and returns them to the
     * available pool. Useful for scene transitions or pause functionality.
     */
    void StopAllSounds();

    /**
     * @brief Pause all currently playing sounds
     * 
     * Pauses all active audio sources without stopping them. Sounds can
     * be resumed later with ResumeAllSounds().
     */
    void PauseAllSounds();

    /**
     * @brief Resume all paused sounds
     * 
     * Resumes playback of all previously paused audio sources.
     */
    void ResumeAllSounds();

    /**
     * @brief Set the master volume level
     * 
     * Adjusts the overall volume for all audio output. This affects all
     * sound categories (SFX, music, etc.).
     * 
     * @param volume Master volume level (0.0 = silent, 1.0 = full volume)
     */
    void SetMasterVolume(float volume);

    /**
     * @brief Set the sound effects volume level
     * 
     * Adjusts the volume level specifically for sound effects, allowing
     * separate control from music and other audio categories.
     * 
     * @param volume SFX volume level (0.0 = silent, 1.0 = full volume)
     */
    void SetSFXVolume(float volume);

    /**
     * @brief Set the music volume level
     * 
     * Adjusts the volume level specifically for music tracks, allowing
     * separate control from sound effects and other audio categories.
     * 
     * @param volume Music volume level (0.0 = silent, 1.0 = full volume)
     */
    void SetMusicVolume(float volume);

    /**
     * @brief Get the number of currently active audio sources
     * 
     * Returns the count of audio sources that are currently playing sounds.
     * Useful for debugging and performance monitoring.
     * 
     * @return Number of active audio sources
     */
    size_t GetActiveSourceCount() const;

    // ============================================================================
    // MIXER, LISTENER AND SOURCE-LIFETIME API
    // ============================================================================

    /**
     * @brief Attach a mix bus provider used to scale category volumes
     *
     * When a mixer is attached, the effective volume of the "SFX" / "Music" bus
     * (including mute, solo and the parent chain) multiplies the category volume
     * for every source, so bus changes become audible. It also supplies audio
     * occlusion for 3D sources when the mixer has a physics system.
     *
     * @param mixer Non-owning mixer pointer, or nullptr to detach
     */
    void SetMixer(Spark::Audio::AudioMixer* mixer);

    /**
     * @brief Effective volume multiplier for a category
     *
     * Returns categoryVolume * mixer bus volume (1.0 when no mixer is attached).
     * Master volume is deliberately excluded: it lives on the mastering voice.
     *
     * @param category Category to query
     * @return Multiplier applied to a caller's requested volume
     */
    float GetCategoryVolume(AudioCategory category) const;

    /**
     * @brief Drive the 3D listener from a camera transform
     *
     * Sets listener position/orientation and derives listener velocity from the
     * position delta. A delta implying a speed above the speed of sound is
     * treated as a teleport discontinuity and yields zero velocity rather than a
     * degenerate Doppler shift.
     *
     * @param position World-space camera position
     * @param forward  Camera forward vector (need not be normalized)
     * @param up       Camera up vector (need not be normalized)
     * @param deltaTime Seconds since the previous call; <= 0 skips velocity
     */
    void SetListenerFromCamera(const XMFLOAT3& position, const XMFLOAT3& forward, const XMFLOAT3& up, float deltaTime);

    /**
     * @brief Set the listener velocity used for Doppler directly
     * @param velocity World-space listener velocity in m/s
     */
    void SetListenerVelocity(const XMFLOAT3& velocity);

    /** @brief Get the current 3D listener position. */
    XMFLOAT3 GetListenerPosition() const;

    /** @brief Get the current 3D listener velocity. */
    XMFLOAT3 GetListenerVelocity() const;

    /**
     * @brief Check whether a handle still names the same live playback
     *
     * Pooled sources are recycled, so a stored AudioSource* can silently start
     * referring to a different sound. Callers that keep a handle must also keep
     * the AudioSource::Generation observed when they acquired it.
     *
     * @param source     Source pointer previously returned by PlaySound*
     * @param generation Generation observed when the handle was stored
     * @return true when the source is still playing that same acquisition
     */
    bool IsSourceLive(const AudioSource* source, uint32_t generation) const;

    /**
     * @brief Distance attenuation for a 3D source (inverse-distance, clamped)
     *
     * Full volume within minDistance, silent beyond maxDistance, inverse
     * rolloff in between scaled by the engine's distance scale.
     *
     * @param distance      Listener-to-source distance in meters
     * @param minDistance   Distance at which attenuation begins
     * @param maxDistance   Distance at which the source is silent
     * @param distanceScale Global rolloff multiplier
     * @return Attenuation in [0, 1]
     */
    static float ComputeDistanceAttenuation(float distance, float minDistance, float maxDistance, float distanceScale);

    /**
     * @brief Whether two wave formats can share a single XAudio2 source voice
     *
     * A source voice is created for one format and cannot be re-pointed at
     * another, so a pooled voice must be destroyed when the format changes.
     */
    static bool FormatsCompatible(const WAVEFORMATEX& a, const WAVEFORMATEX& b);

    /**
     * @brief Whether an XAudio2 HRESULT means the output device went away
     * @param hr HRESULT returned by an XAudio2 call
     */
    static bool IsDeviceLostResult(HRESULT hr);

    /** @brief True once a voice call reported the output device was invalidated. */
    bool IsDeviceLost() const { return m_deviceLost; }

    /**
     * @brief Report a critical audio-engine error (device loss) from any thread.
     *
     * Raised by the registered XAudio2 engine callback when the output device is torn
     * out from under sounds that are already playing - the case no voice-call HRESULT
     * on the game thread would ever reveal. Only a flag is set here; Update() enters
     * the device-lost state and drives recovery on the game thread.
     *
     * @note Thread-safe. Also lets a test drive the device-loss path with no device.
     */
    void ReportCriticalError() noexcept;

    /**
     * @brief Whether playback can currently reach an output device
     *
     * False before Initialize(), after Shutdown(), and while the device is lost.
     */
    bool IsAvailable() const;

    /**
     * @brief Rebuild the mastering voice after a device loss
     *
     * Destroys every voice created against the dead device and recreates the
     * mastering voice. Called automatically (throttled) from Update().
     *
     * @return true when the engine has a usable mastering voice again
     */
    bool RecoverDevice();

    // ============================================================================
    // CONSOLE INTEGRATION METHODS - Audio Engine Control
    // ============================================================================

    /**
     * @brief Audio metrics structure for console integration
     */
    struct AudioMetrics
    {
        size_t activeSources;      ///< Number of currently active audio sources
        size_t totalSources;       ///< Total number of available audio sources
        size_t loadedSounds;       ///< Number of loaded sound effects
        float masterVolume;        ///< Current master volume level
        float sfxVolume;           ///< Current SFX volume level
        float musicVolume;         ///< Current music volume level
        float cpuUsage;            ///< Audio CPU usage percentage
        size_t memoryUsage;        ///< Audio memory usage in bytes
        bool is3DEnabled;          ///< Whether 3D audio is enabled
        XMFLOAT3 listenerPosition; ///< Current listener position
        XMFLOAT3 listenerVelocity; ///< Current listener velocity
        float dopplerScale;        ///< Doppler effect scale factor
        float distanceScale;       ///< Distance attenuation scale factor
    };

    /**
     * @brief Audio settings structure for console control
     */
    struct AudioSettings
    {
        float masterVolume;        ///< Master volume level (0.0-1.0)
        float sfxVolume;           ///< SFX volume level (0.0-1.0)
        float musicVolume;         ///< Music volume level (0.0-1.0)
        float dopplerScale;        ///< Doppler effect scale (0.0-2.0)
        float distanceScale;       ///< Distance attenuation scale (0.1-10.0)
        bool enable3D;             ///< Enable/disable 3D audio processing
        bool enableReverb;         ///< Enable/disable reverb effects
        bool enableEAX;            ///< Enable/disable EAX environmental audio
        int maxSources;            ///< Maximum simultaneous audio sources
        XMFLOAT3 listenerPosition; ///< 3D listener position
        XMFLOAT3 listenerVelocity; ///< 3D listener velocity
        XMFLOAT3 listenerForward;  ///< 3D listener forward direction
        XMFLOAT3 listenerUp;       ///< 3D listener up direction
    };

    /**
     * @brief Set master volume via console
     * @param volume Master volume level (0.0-1.0)
     */
    void Console_SetMasterVolume(float volume);

    /**
     * @brief Set SFX volume via console
     * @param volume SFX volume level (0.0-1.0)
     */
    void Console_SetSFXVolume(float volume);

    /**
     * @brief Set music volume via console
     * @param volume Music volume level (0.0-1.0)
     */
    void Console_SetMusicVolume(float volume);

    /**
     * @brief Set 3D listener position via console
     * @param x X coordinate
     * @param y Y coordinate
     * @param z Z coordinate
     */
    void Console_SetListenerPosition(float x, float y, float z);

    /**
     * @brief Set 3D listener orientation via console
     * @param forwardX Forward vector X component
     * @param forwardY Forward vector Y component
     * @param forwardZ Forward vector Z component
     * @param upX Up vector X component
     * @param upY Up vector Y component
     * @param upZ Up vector Z component
     */
    void Console_SetListenerOrientation(float forwardX, float forwardY, float forwardZ, float upX, float upY,
                                        float upZ);

    /**
     * @brief Set Doppler effect scale via console
     * @param scale Doppler scale factor (0.0-2.0)
     */
    void Console_SetDopplerScale(float scale);

    /**
     * @brief Set distance attenuation scale via console
     * @param scale Distance attenuation scale (0.1-10.0)
     */
    void Console_SetDistanceScale(float scale);

    /**
     * @brief Enable/disable 3D audio processing via console
     * @param enabled true to enable 3D audio, false to disable
     */
    void Console_Set3DAudio(bool enabled);

    /**
     * @brief Play a test sound via console
     * @param soundName Name of sound to play
     * @param is3D Whether to play as 3D sound
     * @return ID of the playing sound source
     */
    uint32_t Console_PlayTestSound(const std::string& soundName, bool is3D = false);

    /**
     * @brief Stop a specific sound source via console
     * @param sourceID ID of the sound source to stop
     */
    void Console_StopSound(uint32_t sourceID);

    /**
     * @brief Stop all playing sounds via console
     */
    void Console_StopAllSounds();

    /**
     * @brief List all loaded sounds via console
     * @return String containing list of all loaded sounds
     */
    std::string Console_ListSounds() const;

    /**
     * @brief Get comprehensive audio metrics (console integration)
     * @return AudioMetrics structure with current audio data
     */
    AudioMetrics Console_GetMetrics() const;

    /**
     * @brief Get current audio settings (console integration)
     * @return AudioSettings structure with current settings
     */
    AudioSettings Console_GetSettings() const;

    /**
     * @brief Apply audio settings from console
     * @param settings AudioSettings structure with new settings
     */
    void Console_ApplySettings(const AudioSettings& settings);

    /**
     * @brief Reset audio settings to defaults via console
     */
    void Console_ResetToDefaults();

    /**
     * @brief Register console state change callback
     * @param callback Function to call when audio state changes
     */
    void Console_RegisterStateCallback(std::function<void()> callback);

    /**
     * @brief Create a submix voice for audio bus/group routing
     *
     * Creates an XAudio2 submix voice that can be used as an intermediate
     * mixing stage between source voices and the mastering voice.
     *
     * @param inputChannels Number of input channels (default: 2 for stereo)
     * @param inputSampleRate Sample rate in Hz (default: 44100)
     * @return Pointer to the created submix voice, or nullptr on failure
     *
     * @warning A returned pointer does not survive an output-device loss: recovery
     * destroys every voice created against the dead device and creates a replacement
     * with the same parameters. Re-query after IsDeviceLost() has cleared instead of
     * caching the pointer across frames.
     */
    IXAudio2SubmixVoice* CreateSubmixVoice(uint32_t inputChannels = 2, uint32_t inputSampleRate = 44100);

    /**
     * @brief Get the master volume level
     * @return Current master volume (0.0 to 1.0)
     */
    float GetMasterVolume() const { return m_masterVolume; }

    /**
     * @brief Get the SFX volume level
     * @return Current SFX volume (0.0 to 1.0)
     */
    float GetSFXVolume() const { return m_sfxVolume; }

    /**
     * @brief Get the music volume level
     * @return Current music volume (0.0 to 1.0)
     */
    float GetMusicVolume() const { return m_musicVolume; }

    /**
     * @brief Get the XAudio2 engine interface
     * @return Pointer to the IXAudio2 engine, or nullptr if not initialized
     */
    IXAudio2* GetXAudio2() const { return m_xAudio2; }

    /**
     * @brief Get the mastering voice
     * @return Pointer to the mastering voice, or nullptr if not initialized
     */
    IXAudio2MasteringVoice* GetMasteringVoice() const { return m_masterVoice; }

    /**
     * @brief Force audio system refresh via console
     */
    void Console_RefreshAudio();

    /**
     * @brief Get audio source information via console
     * @param sourceID ID of the audio source
     * @return String containing source information
     */
    std::string Console_GetSourceInfo(uint32_t sourceID) const;

  private:
    /**
     * @brief Create an XAudio2 source voice for audio playback
     * 
     * Internal method for creating source voices with the specified audio format.
     * 
     * @param format Audio format description for the source voice
     * @param voice Output parameter for the created source voice
     * @return HRESULT indicating success or failure of voice creation
     */
    HRESULT CreateSourceVoice(const WAVEFORMATEX& format, IXAudio2SourceVoice** voice);

    /**
     * @brief Update all active audio sources
     * 
     * Processes 3D audio calculations, updates source voice parameters,
     * and handles audio source lifecycle management.
     */
    void UpdateSources();

    /**
     * @brief Get an available audio source from the pool
     * 
     * Retrieves an unused audio source for playing a new sound. If no
     * sources are available, may stop the oldest playing source.
     * 
     * @return Pointer to an available AudioSource, or nullptr if none available
     */
    AudioSource* GetAvailableSource();

    /**
     * @brief Return an audio source to the available pool
     * 
     * Marks an audio source as available for reuse and cleans up its state.
     * 
     * @param source Pointer to the AudioSource to return to the pool
     */
    void ReturnSource(AudioSource* source);

    /**
     * @brief Apply 3D audio calculations to a specific source
     * 
     * Calculates distance attenuation, stereo panning, and Doppler effects
     * for a 3D positioned audio source.
     * 
     * @param source Pointer to the AudioSource to apply 3D calculations to
     */
    void Apply3DAudioToSource(AudioSource* source);

    /**
     * @brief Update 3D audio calculations
     */
    void Update3DAudio();

    /**
     * @brief Push a source's category-scaled gain to its voice
     * @param source Source whose RequestedVolume/Category should be re-applied
     */
    void ApplySourceGain(AudioSource& source);

    /**
     * @brief Re-apply gains to every playing source in a category
     * @param category Category whose live sources must pick up a new volume
     */
    void RefreshSourceGains(AudioCategory category);

    /**
     * @brief Record a device-invalidated HRESULT so Update() can recover
     * @param hr HRESULT returned by an XAudio2 voice call
     */
    void HandleVoiceResult(HRESULT hr);

    /**
     * @brief Enter the device-lost state and start the recovery throttle
     *
     * @note [game thread] Called from HandleVoiceResult() and from Update() when the
     * XAudio2 engine callback reported a critical error on its own worker thread.
     */
    void HandleDeviceLoss();

#ifdef SPARK_PLATFORM_WINDOWS
    /**
     * @brief XAudio2 engine callback used to observe device loss.
     *
     * Voice-call HRESULTs only reveal a vanished output device when the game calls
     * into XAudio2. A session that is merely keeping already-started sounds playing
     * issues no such call, so without this callback the device could die while
     * IsAvailable() kept reporting true and no recovery was ever attempted.
     *
     * @note OnCriticalError runs on the XAudio2 worker thread: it only raises an
     * atomic flag that Update() consumes on the game thread.
     */
    class DeviceLossCallback final : public IXAudio2EngineCallback
    {
      public:
        explicit DeviceLossCallback(AudioEngine& owner) noexcept : m_owner(owner) {}

        void STDMETHODCALLTYPE OnProcessingPassStart() override {}
        void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override;

      private:
        AudioEngine& m_owner;
    };

    DeviceLossCallback m_deviceLossCallback{*this}; ///< Registered with IXAudio2 in Initialize()
#endif                                              // SPARK_PLATFORM_WINDOWS

    /// Raised by the XAudio2 worker thread, consumed by Update() on the game thread.
    std::atomic<bool> m_criticalErrorPending{false};

    /**
     * @brief Notify console of state changes
     */
    void NotifyStateChange();

    /**
     * @brief Thread-safe metrics access helper
     * @return Current audio metrics with thread safety
     */
    AudioMetrics GetMetricsThreadSafe() const;

    /**
     * @brief One created submix voice plus the parameters needed to recreate it.
     *
     * Device recovery destroys every voice created against the dead device; keeping the
     * creation parameters is what lets the submixes come back instead of silently
     * disappearing for the rest of the session.
     */
    struct SubmixVoiceRecord
    {
        IXAudio2SubmixVoice* voice = nullptr; ///< Live voice (owned by XAudio2)
        uint32_t inputChannels = 0;           ///< Channel count the voice was created with
        uint32_t inputSampleRate = 0;         ///< Sample rate the voice was created with
    };

    IXAudio2* m_xAudio2;                             ///< Main XAudio2 engine interface
    IXAudio2MasteringVoice* m_masterVoice;           ///< XAudio2 mastering voice for final output
    std::vector<SubmixVoiceRecord> m_submixVoices;   ///< Created submix voices for cleanup/recovery
    Spark::Audio::AudioMixer* m_mixer;                ///< Non-owning mix bus provider (may be null)
    bool m_deviceLost;                                ///< Output device reported invalidated
    float m_deviceRecoveryTimer;                      ///< Seconds since the last recovery attempt
    float m_masterVolume;                             ///< Current master volume level
    float m_sfxVolume;                                ///< Current sound effects volume level
    float m_musicVolume;                              ///< Current music volume level
    size_t m_maxSources;                              ///< Maximum number of simultaneous sources

    std::vector<std::unique_ptr<AudioSource>> m_audioSources;                     ///< Pool of all audio sources
    std::vector<AudioSource*> m_availableSources;                                 ///< Pool of available sources
    std::unordered_map<std::string, std::unique_ptr<SoundEffect>> m_soundEffects; ///< Loaded sound effects by name

    // Console integration state
    AudioSettings m_settings;          ///< Current audio settings
    mutable std::mutex m_metricsMutex; ///< Thread safety for metrics access
    std::mutex m_sourceMutex;
    std::function<void()> m_stateCallback; ///< Callback for state changes
    uint32_t m_nextSourceID;               ///< Next unique source ID

    // 3D Audio state
    XMFLOAT3 m_listenerPosition; ///< Current listener position
    XMFLOAT3 m_listenerVelocity; ///< Current listener velocity
    XMFLOAT3 m_listenerForward;  ///< Current listener forward direction
    XMFLOAT3 m_listenerUp;       ///< Current listener up direction
    float m_dopplerScale;        ///< Doppler effect scale factor
    float m_distanceScale;       ///< Distance attenuation scale
    bool m_3DEnabled;            ///< Whether 3D audio is enabled
};
