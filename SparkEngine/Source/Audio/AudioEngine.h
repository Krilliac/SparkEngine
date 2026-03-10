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
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

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
    float Volume;               ///< Volume level (0.0 to 1.0+)
    float Pitch;                ///< Pitch multiplier (1.0 = normal pitch)
    bool Is3D;                  ///< Whether this source uses 3D positioning
    bool IsLooping;             ///< Whether the sound should loop continuously
    bool IsPlaying;             ///< Whether the source is currently playing
    SoundEffect* Sound;         ///< Pointer to the associated sound effect
    uint32_t SourceID;          ///< Unique identifier for console tracking

    /**
     * @brief Default constructor with safe initial values
     * 
     * Initializes all members to safe defaults suitable for audio playback.
     */
    AudioSource()
        : Voice(nullptr), Position(0, 0, 0), Velocity(0, 0, 0), Volume(1.0f), Pitch(1.0f), Is3D(false),
          IsLooping(false), IsPlaying(false), Sound(nullptr), SourceID(0)
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
     * @return Pointer to the AudioSource playing the sound, or nullptr if failed
     */
    AudioSource* PlaySound(const std::string& name, float volume = 1.0f, float pitch = 1.0f, bool loop = false);

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
     * @return Pointer to the AudioSource playing the sound, or nullptr if failed
     */
    AudioSource* PlaySound3D(const std::string& name, const XMFLOAT3& position, float volume = 1.0f, float pitch = 1.0f,
                             bool loop = false);

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
     */
    IXAudio2SubmixVoice* CreateSubmixVoice(UINT32 inputChannels = 2, UINT32 inputSampleRate = 44100);

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
     * @brief Notify console of state changes
     */
    void NotifyStateChange();

    /**
     * @brief Thread-safe metrics access helper
     * @return Current audio metrics with thread safety
     */
    AudioMetrics GetMetricsThreadSafe() const;

    IXAudio2* m_xAudio2;                              ///< Main XAudio2 engine interface
    IXAudio2MasteringVoice* m_masterVoice;            ///< XAudio2 mastering voice for final output
    std::vector<IXAudio2SubmixVoice*> m_submixVoices; ///< Created submix voices for cleanup
    float m_masterVolume;                  ///< Current master volume level
    float m_sfxVolume;                     ///< Current sound effects volume level
    float m_musicVolume;                   ///< Current music volume level
    size_t m_maxSources;                   ///< Maximum number of simultaneous sources

    std::vector<std::unique_ptr<AudioSource>> m_audioSources;                     ///< Pool of all audio sources
    std::vector<AudioSource*> m_availableSources;                                 ///< Pool of available sources
    std::unordered_map<std::string, std::unique_ptr<SoundEffect>> m_soundEffects; ///< Loaded sound effects by name

    // Console integration state
    AudioSettings m_settings;              ///< Current audio settings
    mutable std::mutex m_metricsMutex;     ///< Thread safety for metrics access
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
