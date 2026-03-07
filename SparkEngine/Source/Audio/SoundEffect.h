/**
 * @file SoundEffect.h
 * @brief Sound effect loading and procedural audio generation system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file provides comprehensive sound effect functionality including WAV file
 * loading, in-memory audio buffer management, and a factory system for generating
 * procedural sound effects. The system is designed to work efficiently with XAudio2
 * and provides both file-based and procedural audio capabilities.
 */

#pragma once
#include "../Core/Platform.h"

//------------------------------------------------------------------------------
//  SoundEffect ‒ simple WAV loader + in-memory buffer
//  Depends only on <xaudio2.h> and the custom Assert system.
//------------------------------------------------------------------------------

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <xaudio2.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <vector>
#include <string>
#include <memory>

/**
 * @brief Sound effect class for loading and managing audio data
 * 
 * The SoundEffect class provides functionality for loading WAV audio files,
 * storing audio data in memory, and providing access to audio format information.
 * It supports both file-based loading and direct memory loading for procedural
 * or streamed audio content.
 * 
 * Features include:
 * - WAV file format parsing and loading
 * - In-memory audio buffer management
 * - Audio format information access
 * - Duration and sample rate queries
 * - Memory-efficient storage with automatic cleanup
 * 
 * @note Currently supports WAV format audio files
 * @warning Ensure audio files are in supported formats before loading
 */
class SoundEffect
{
public:
    /**
     * @brief Default constructor
     * 
     * Initializes the sound effect with empty audio data and default format values.
     */
    SoundEffect();

    /**
     * @brief Destructor
     * 
     * Automatically calls Unload() to ensure proper cleanup of audio resources.
     */
    ~SoundEffect();

    /**
     * @brief Load audio data from a WAV file
     * 
     * Loads and parses a WAV audio file, storing the audio data and format
     * information for later playback through XAudio2.
     * 
     * @param filename Path to the WAV file to load
     * @return HRESULT indicating success or failure of loading operation
     * @note Only WAV format files are currently supported
     */
    HRESULT LoadFromFile(const std::wstring& filename);

    /**
     * @brief Load audio data from memory buffer
     * 
     * Parses WAV format audio data from a memory buffer, useful for embedded
     * audio resources or procedurally generated audio content.
     * 
     * @param data Pointer to the audio data in WAV format
     * @param dataSize Size of the audio data in bytes
     * @return HRESULT indicating success or failure of parsing operation
     */
    HRESULT LoadFromMemory(const BYTE* data, DWORD dataSize);

    /**
     * @brief Unload audio data and free memory
     * 
     * Releases all audio data and resets the sound effect to an empty state.
     * Safe to call multiple times.
     */
    void    Unload();

    /**
     * @brief Get the audio format structure
     * @return Reference to the WAVEFORMATEX structure describing the audio format
     */
    const WAVEFORMATEX& GetFormat()   const { return m_format; }

    /**
     * @brief Get pointer to the raw audio data
     * @return Pointer to the audio sample data, or nullptr if not loaded
     */
    const BYTE* GetData()     const { return m_audioData.data(); }

    /**
     * @brief Get the size of the audio data in bytes
     * @return Size of the audio data buffer in bytes
     */
    DWORD              GetDataSize() const { return m_audioDataSize; }

    /**
     * @brief Check if audio data is currently loaded
     * @return true if audio data is loaded, false if empty
     */
    bool               IsLoaded()    const { return m_audioDataSize != 0; }

    /**
     * @brief Get the duration of the audio in seconds
     * @return Duration of the audio clip in seconds
     */
    float  GetDuration()     const;                 // seconds

    /**
     * @brief Get the sample rate of the audio
     * @return Sample rate in samples per second (Hz)
     */
    DWORD  GetSampleRate()   const { return m_format.nSamplesPerSec; }

    /**
     * @brief Get the number of audio channels
     * @return Number of channels (1 = mono, 2 = stereo, etc.)
     */
    WORD   GetChannels()     const { return m_format.nChannels; }

    /**
     * @brief Get the bit depth of the audio samples
     * @return Bits per sample (typically 8, 16, or 24)
     */
    WORD   GetBitsPerSample()const { return m_format.wBitsPerSample; }

private:
    WAVEFORMATEX              m_format;         ///< Audio format description
    std::vector<BYTE>         m_audioData;      ///< Raw audio sample data
    DWORD                     m_audioDataSize;  ///< Size of audio data in bytes

    /**
     * @brief Parse WAV file format from memory buffer
     * 
     * Internal method that parses WAV file headers and extracts audio data
     * and format information from a memory buffer.
     * 
     * @param data Pointer to WAV file data
     * @param size Size of the WAV file data
     * @return HRESULT indicating success or failure of parsing
     */
    HRESULT ParseWAVFile(const BYTE* data, DWORD size);

    /**
     * @brief Find a specific chunk in WAV file data
     * 
     * Searches for a chunk with the specified FourCC identifier in WAV file data.
     * 
     * @param data Pointer to WAV file data
     * @param dataSize Size of the WAV file data
     * @param fourCC Four-character code identifying the chunk type
     * @param outSize Output parameter for chunk size
     * @param outPos Output parameter for chunk position
     * @return HRESULT indicating success or failure of chunk location
     */
    HRESULT FindChunk(const BYTE* data, DWORD dataSize,
        DWORD fourCC, DWORD& outSize, DWORD& outPos);

    /**
     * @brief Read data from a located chunk
     * 
     * Reads the specified number of bytes from a chunk at the given position.
     * 
     * @param data Pointer to source data
     * @param pos Position within the data to start reading
     * @param out Output buffer for read data
     * @param bytes Number of bytes to read
     * @return HRESULT indicating success or failure of read operation
     */
    HRESULT ReadChunkData(const BYTE* data, DWORD pos, void* out, DWORD bytes);

    // grant factory access to internals
    friend class SoundEffectFactory;
};

/**
 * @brief Factory class for generating procedural sound effects
 * 
 * The SoundEffectFactory provides static methods for creating various types of
 * procedural sound effects without requiring external audio files. This is useful
 * for placeholder sounds, debugging, or games that prefer procedural audio.
 * 
 * Features include:
 * - Basic waveform generation (sine, beep, noise)
 * - Game-specific sound effects (gunshots, explosions, footsteps)
 * - Configurable parameters for frequency and duration
 * - Automatic WAV format wrapping for XAudio2 compatibility
 * 
 * @note All generated sounds are in standard PCM WAV format
 * @warning Generated sounds are synthetic and may not be suitable for final game audio
 */
class SoundEffectFactory
{
public:
    /**
     * @brief Create a simple beep tone
     * 
     * Generates a basic square wave beep sound with the specified frequency and duration.
     * 
     * @param freq Frequency of the beep in Hz (default: 440Hz - A4 note)
     * @param dur Duration of the beep in seconds (default: 0.5 seconds)
     * @return Unique pointer to the generated SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreateBeep(float freq = 440.f, float dur = 0.5f);

    /**
     * @brief Create a smooth sine wave tone
     * 
     * Generates a pure sine wave tone with the specified frequency and duration.
     * 
     * @param freq Frequency of the sine wave in Hz (default: 440Hz)
     * @param dur Duration of the tone in seconds (default: 1.0 second)
     * @return Unique pointer to the generated SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreateSine(float freq = 440.f, float dur = 1.0f);

    /**
     * @brief Create white noise
     * 
     * Generates random white noise for the specified duration.
     * 
     * @param dur Duration of the noise in seconds (default: 1.0 second)
     * @return Unique pointer to the generated SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreateNoise(float dur = 1.0f);

    /**
     * @brief Create a gunshot sound effect
     * 
     * Generates a procedural gunshot sound with appropriate frequency characteristics
     * and envelope for realistic weapon audio.
     * 
     * @return Unique pointer to the generated gunshot SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreateGunshot();

    /**
     * @brief Create an explosion sound effect
     * 
     * Generates a procedural explosion sound with low frequency rumble and
     * high frequency crack components.
     * 
     * @return Unique pointer to the generated explosion SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreateExplosion();

    /**
     * @brief Create a footstep sound effect
     * 
     * Generates a procedural footstep sound suitable for character movement audio.
     * 
     * @return Unique pointer to the generated footstep SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreateFootstep();

    /**
     * @brief Create a weapon reload sound effect
     * 
     * Generates a procedural reload sound with metallic clicking characteristics.
     * 
     * @return Unique pointer to the generated reload SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreateReload();

    /**
     * @brief Create an item pickup sound effect
     * 
     * Generates a pleasant pickup sound suitable for item collection feedback.
     * 
     * @return Unique pointer to the generated pickup SoundEffect
     */
    static std::unique_ptr<SoundEffect> CreatePickup();

private:
    /**
     * @brief Generate waveform samples using a wave function
     * 
     * Internal method for generating audio samples using mathematical wave functions.
     * 
     * @param dst Output vector for generated samples
     * @param freq Frequency of the waveform in Hz
     * @param dur Duration of the waveform in seconds
     * @param wave Function pointer to the wave generation function
     */
    static void  GenerateWaveform(std::vector<short>& dst,
        float freq, float dur,
        float (*wave)(float));

    /**
     * @brief Create a SoundEffect from sample data
     * 
     * Internal method for wrapping raw sample data in a SoundEffect object
     * with proper WAV format headers.
     * 
     * @param samples Vector of 16-bit audio samples
     * @param sampleRate Sample rate for the audio (default: 44100 Hz)
     * @return Unique pointer to the created SoundEffect
     */
    static std::unique_ptr<SoundEffect>
        CreateFromSamples(const std::vector<short>& samples,
            DWORD sampleRate = 44100);

    /**
     * @brief Generate sine wave sample
     * @param phase Phase value for sine calculation (0 to 2π)
     * @return Sine wave sample value
     */
    static float SineWave(float phase);

    /**
     * @brief Generate noise sample
     * @param phase Phase value (ignored for noise)
     * @return Random noise sample value
     */
    static float NoiseWave(float);      // phase ignored
};
