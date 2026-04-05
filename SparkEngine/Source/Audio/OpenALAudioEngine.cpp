/**
 * @file OpenALAudioEngine.cpp
 * @brief OpenAL Soft audio backend implementation for Linux/macOS
 *
 * Provides real audio output on non-Windows platforms using OpenAL Soft.
 * Supports WAV file loading, 3D spatial audio with distance attenuation,
 * Doppler effects, and efficient source pooling.
 */

#include "OpenALAudioEngine.h"
#include "../Core/Platform.h"

#if !defined(SPARK_PLATFORM_WINDOWS)

#include "../Utils/Validate.h"

#ifdef SPARK_OPENAL_AVAILABLE
#include <AL/al.h>
#include <AL/alc.h>
#else
// Stub typedefs when OpenAL is not available - all operations become no-ops
typedef int ALuint;
typedef int ALenum;
typedef int ALint;
typedef float ALfloat;
typedef int ALsizei;
typedef char ALchar;
typedef bool ALboolean;
#define AL_NO_ERROR 0
#define AL_SOURCE_STATE 0
#define AL_PLAYING 1
#define AL_PAUSED 2
#define AL_STOPPED 3
#define AL_POSITION 4
#define AL_VELOCITY 5
#define AL_GAIN 6
#define AL_PITCH 7
#define AL_LOOPING 8
#define AL_BUFFER 9
#define AL_ORIENTATION 10
#define AL_FORMAT_MONO8 0x1100
#define AL_FORMAT_MONO16 0x1101
#define AL_FORMAT_STEREO8 0x1102
#define AL_FORMAT_STEREO16 0x1103
#define AL_TRUE 1
#define AL_FALSE 0
#define AL_DISTANCE_MODEL 0xD000
#define AL_INVERSE_DISTANCE_CLAMPED 0xD002
#define AL_DOPPLER_FACTOR 0xC000
#define AL_SPEED_OF_SOUND 0xC003
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Spark::Audio
{

    // ============================================================================
    // WAV file parsing helpers
    // ============================================================================

    struct WAVHeader
    {
        char riffId[4];
        uint32_t riffSize;
        char waveId[4];
    };

    struct WAVChunkHeader
    {
        char id[4];
        uint32_t size;
    };

    struct WAVFmtChunk
    {
        uint16_t audioFormat;
        uint16_t numChannels;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample;
    };

    // ============================================================================
    // OpenALAudioEngine implementation
    // ============================================================================

    OpenALAudioEngine::OpenALAudioEngine() = default;

    OpenALAudioEngine::~OpenALAudioEngine()
    {
        Shutdown();
    }

    bool OpenALAudioEngine::Initialize(size_t maxSources)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
#ifdef SPARK_OPENAL_AVAILABLE
        m_maxSources = maxSources;

        // Open default audio device
        m_device = alcOpenDevice(nullptr);
        if (!m_device)
        {
            fprintf(stderr, "[OpenAL] Failed to open audio device\n");
            return false;
        }

        // Create audio context
        m_context = alcCreateContext(m_device, nullptr);
        if (!m_context)
        {
            fprintf(stderr, "[OpenAL] Failed to create audio context\n");
            alcCloseDevice(m_device);
            m_device = nullptr;
            return false;
        }

        alcMakeContextCurrent(m_context);

        // Configure 3D audio model
        alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
        alDopplerFactor(m_dopplerScale);
        alSpeedOfSound(343.3f);

        // Set initial listener position
        alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
        alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
        float orientation[] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
        alListenerfv(AL_ORIENTATION, orientation);

        // Create source pool
        m_sources.clear();
        m_availableSources.clear();
        m_sources.reserve(m_maxSources);
        m_availableSources.reserve(m_maxSources);

        for (size_t i = 0; i < m_maxSources; ++i)
        {
            auto src = std::make_unique<OpenALSource>();
            alGenSources(1, &src->alSource);
            if (alGetError() != AL_NO_ERROR)
            {
                fprintf(stderr, "[OpenAL] Failed to generate source %zu/%zu\n", i, m_maxSources);
                break;
            }
            src->SourceID = m_nextSourceID++;
            m_availableSources.push_back(src.get());
            m_sources.push_back(std::move(src));
        }

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Audio, "OpenAL audio engine initialized: %zu sources", m_sources.size());
        fprintf(stdout, "[OpenAL] Audio engine initialized: %zu sources, device: %s\n", m_sources.size(),
                alcGetString(m_device, ALC_DEVICE_SPECIFIER));
        return true;
#else
        (void)maxSources;
        fprintf(stderr, "[OpenAL] OpenAL Soft not available - audio disabled\n");
        return false;
#endif
    }

    void OpenALAudioEngine::Update(float /*deltaTime*/)
    {
        if (!m_initialized)
            return;
        UpdateSources();
    }

    void OpenALAudioEngine::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
#ifdef SPARK_OPENAL_AVAILABLE
        if (!m_initialized)
            return;

        StopAllSounds();

        // Delete OpenAL sources
        for (auto& src : m_sources)
        {
            if (src->alSource)
            {
                alDeleteSources(1, &src->alSource);
                src->alSource = 0;
            }
        }
        m_sources.clear();
        m_availableSources.clear();

        // Delete OpenAL buffers
        for (auto& [name, buf] : m_sounds)
        {
            if (buf.alBuffer)
            {
                alDeleteBuffers(1, &buf.alBuffer);
                buf.alBuffer = 0;
            }
        }
        m_sounds.clear();

        // Destroy context and device
        alcMakeContextCurrent(nullptr);
        if (m_context)
        {
            alcDestroyContext(m_context);
            m_context = nullptr;
        }
        if (m_device)
        {
            alcCloseDevice(m_device);
            m_device = nullptr;
        }

        m_initialized = false;
        SPARK_LOG_INFO(Spark::LogCategory::Audio, "OpenAL audio engine shut down");
        fprintf(stdout, "[OpenAL] Audio engine shut down\n");
#endif
    }

    bool OpenALAudioEngine::LoadSound(const std::string& name, const std::string& filename)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
        SPARK_WARN_IF(Spark::LogCategory::Audio, name.empty(), "LoadSound called with empty name");
        SPARK_WARN_IF(Spark::LogCategory::Audio, filename.empty(), "LoadSound called with empty filename");
#ifdef SPARK_OPENAL_AVAILABLE
        if (!m_initialized)
            return false;

        SoundBuffer buffer;
        if (!LoadWAVFile(filename, buffer))
            return false;

        // Replace existing sound if present
        auto it = m_sounds.find(name);
        if (it != m_sounds.end())
        {
            alDeleteBuffers(1, &it->second.alBuffer);
            it->second = buffer;
        }
        else
        {
            m_sounds[name] = buffer;
        }

        fprintf(stdout, "[OpenAL] Loaded sound '%s': %.2fs, %uHz, %uch\n", name.c_str(), buffer.duration,
                buffer.sampleRate, buffer.channels);
        return true;
#else
        (void)name;
        (void)filename;
        return false;
#endif
    }

    void OpenALAudioEngine::UnloadSound(const std::string& name)
    {
#ifdef SPARK_OPENAL_AVAILABLE
        auto it = m_sounds.find(name);
        if (it == m_sounds.end())
            return;

        // Stop any sources using this buffer
        for (auto& src : m_sources)
        {
            if (src->IsPlaying && src->SoundName == name)
                StopSound(src.get());
        }

        alDeleteBuffers(1, &it->second.alBuffer);
        m_sounds.erase(it);
#else
        (void)name;
#endif
    }

    OpenALSource* OpenALAudioEngine::PlaySound(const std::string& name, float volume, float pitch, bool loop)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
#ifdef SPARK_OPENAL_AVAILABLE
        if (!m_initialized)
            return nullptr;

        auto it = m_sounds.find(name);
        if (it == m_sounds.end())
            return nullptr;

        OpenALSource* src = GetAvailableSource();
        if (!src)
            return nullptr;

        volume = std::clamp(volume, 0.0f, 1.0f);
        if (pitch <= 0.0f)
            pitch = 1.0f;

        src->SoundName = name;
        src->Volume = volume;
        src->Pitch = pitch;
        src->IsLooping = loop;
        src->Is3D = false;
        src->IsPlaying = true;

        float finalVolume = volume * m_sfxVolume * m_masterVolume;

        alSourcei(src->alSource, AL_BUFFER, static_cast<ALint>(it->second.alBuffer));
        alSourcef(src->alSource, AL_GAIN, finalVolume);
        alSourcef(src->alSource, AL_PITCH, pitch);
        alSourcei(src->alSource, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);

        // For 2D sounds, disable 3D processing
        alSourcei(src->alSource, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(src->alSource, AL_POSITION, 0.0f, 0.0f, 0.0f);

        alSourcePlay(src->alSource);
        return src;
#else
        (void)name;
        (void)volume;
        (void)pitch;
        (void)loop;
        return nullptr;
#endif
    }

    OpenALSource* OpenALAudioEngine::PlaySound3D(const std::string& name, const Float3& position, float volume,
                                                 float pitch, bool loop)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
#ifdef SPARK_OPENAL_AVAILABLE
        if (!m_initialized)
            return nullptr;

        auto it = m_sounds.find(name);
        if (it == m_sounds.end())
            return nullptr;

        OpenALSource* src = GetAvailableSource();
        if (!src)
            return nullptr;

        volume = std::clamp(volume, 0.0f, 1.0f);
        if (pitch <= 0.0f)
            pitch = 1.0f;

        src->SoundName = name;
        src->Volume = volume;
        src->Pitch = pitch;
        src->IsLooping = loop;
        src->Is3D = true;
        src->IsPlaying = true;
        src->Position = position;

        float finalVolume = volume * m_sfxVolume * m_masterVolume;

        alSourcei(src->alSource, AL_BUFFER, static_cast<ALint>(it->second.alBuffer));
        alSourcef(src->alSource, AL_GAIN, finalVolume);
        alSourcef(src->alSource, AL_PITCH, pitch);
        alSourcei(src->alSource, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);

        // Enable 3D positioning
        alSourcei(src->alSource, AL_SOURCE_RELATIVE, AL_FALSE);
        alSource3f(src->alSource, AL_POSITION, position.x, position.y, position.z);
        alSourcef(src->alSource, AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(src->alSource, AL_MAX_DISTANCE, 100.0f * m_distanceScale);
        alSourcef(src->alSource, AL_ROLLOFF_FACTOR, m_distanceScale);

        alSourcePlay(src->alSource);
        return src;
#else
        (void)name;
        (void)position;
        (void)volume;
        (void)pitch;
        (void)loop;
        return nullptr;
#endif
    }

    void OpenALAudioEngine::StopSound(OpenALSource* source)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Audio, source);
#ifdef SPARK_OPENAL_AVAILABLE
        if (!source->IsPlaying)
            return;
        alSourceStop(source->alSource);
        alSourcei(source->alSource, AL_BUFFER, 0);
        source->IsPlaying = false;
        ReturnSource(source);
#else
        (void)source;
#endif
    }

    void OpenALAudioEngine::StopAllSounds()
    {
        for (auto& src : m_sources)
        {
            if (src->IsPlaying)
                StopSound(src.get());
        }
    }

    void OpenALAudioEngine::PauseAllSounds()
    {
#ifdef SPARK_OPENAL_AVAILABLE
        for (auto& src : m_sources)
        {
            if (src->IsPlaying)
                alSourcePause(src->alSource);
        }
#endif
    }

    void OpenALAudioEngine::ResumeAllSounds()
    {
#ifdef SPARK_OPENAL_AVAILABLE
        for (auto& src : m_sources)
        {
            if (src->IsPlaying)
                alSourcePlay(src->alSource);
        }
#endif
    }

    void OpenALAudioEngine::SetMasterVolume(float volume)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
#ifdef SPARK_OPENAL_AVAILABLE
        alListenerf(AL_GAIN, m_masterVolume);
#endif
    }

    void OpenALAudioEngine::SetSFXVolume(float volume)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sfxVolume = std::clamp(volume, 0.0f, 1.0f);
    }

    void OpenALAudioEngine::SetMusicVolume(float volume)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_musicVolume = std::clamp(volume, 0.0f, 1.0f);
    }

    void OpenALAudioEngine::SetListenerPosition(const Float3& pos)
    {
#ifdef SPARK_OPENAL_AVAILABLE
        alListener3f(AL_POSITION, pos.x, pos.y, pos.z);
#else
        (void)pos;
#endif
    }

    void OpenALAudioEngine::SetListenerOrientation(const Float3& forward, const Float3& up)
    {
#ifdef SPARK_OPENAL_AVAILABLE
        float orientation[] = {forward.x, forward.y, forward.z, up.x, up.y, up.z};
        alListenerfv(AL_ORIENTATION, orientation);
#else
        (void)forward;
        (void)up;
#endif
    }

    void OpenALAudioEngine::SetListenerVelocity(const Float3& vel)
    {
#ifdef SPARK_OPENAL_AVAILABLE
        alListener3f(AL_VELOCITY, vel.x, vel.y, vel.z);
#else
        (void)vel;
#endif
    }

    void OpenALAudioEngine::SetDopplerScale(float scale)
    {
        m_dopplerScale = std::clamp(scale, 0.0f, 2.0f);
#ifdef SPARK_OPENAL_AVAILABLE
        alDopplerFactor(m_dopplerScale);
#endif
    }

    void OpenALAudioEngine::SetDistanceScale(float scale)
    {
        m_distanceScale = std::clamp(scale, 0.1f, 10.0f);
    }

    void OpenALAudioEngine::Set3DEnabled(bool enabled)
    {
        m_3DEnabled = enabled;
    }

    size_t OpenALAudioEngine::GetActiveSourceCount() const
    {
        size_t count = 0;
        for (const auto& src : m_sources)
        {
            if (src->IsPlaying)
                ++count;
        }
        return count;
    }

    AudioMetrics OpenALAudioEngine::GetMetrics() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        AudioMetrics m;
        m.activeSources = GetActiveSourceCount();
        m.totalSources = m_sources.size();
        m.loadedSounds = m_sounds.size();
        m.masterVolume = m_masterVolume;
        m.sfxVolume = m_sfxVolume;
        m.musicVolume = m_musicVolume;
        m.is3DEnabled = m_3DEnabled;
        m.dopplerScale = m_dopplerScale;
        m.distanceScale = m_distanceScale;
        return m;
    }

    std::string OpenALAudioEngine::Console_ListSounds() const
    {
        std::ostringstream ss;
        ss << "Loaded Sounds (" << m_sounds.size() << " total):\n";
        for (const auto& [name, buf] : m_sounds)
        {
            ss << "  " << name << " - " << buf.duration << "s, " << buf.sampleRate << "Hz, " << buf.channels << "ch\n";
        }
        if (m_sounds.empty())
            ss << "  No sounds loaded\n";
        return ss.str();
    }

    std::string OpenALAudioEngine::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "OpenAL Audio Engine Status:\n";
        ss << "  Initialized: " << (m_initialized ? "YES" : "NO") << "\n";
        ss << "  Active Sources: " << GetActiveSourceCount() << "/" << m_sources.size() << "\n";
        ss << "  Loaded Sounds: " << m_sounds.size() << "\n";
        ss << "  Master Volume: " << m_masterVolume << "\n";
        ss << "  SFX Volume: " << m_sfxVolume << "\n";
        ss << "  Music Volume: " << m_musicVolume << "\n";
        ss << "  3D Audio: " << (m_3DEnabled ? "ON" : "OFF") << "\n";
        return ss.str();
    }

    // ============================================================================
    // Private helpers
    // ============================================================================

    bool OpenALAudioEngine::LoadWAVFile(const std::string& filename, SoundBuffer& outBuffer)
    {
#ifdef SPARK_OPENAL_AVAILABLE
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open())
        {
            fprintf(stderr, "[OpenAL] Failed to open file: %s\n", filename.c_str());
            return false;
        }

        // Read entire file
        file.seekg(0, std::ios::end);
        auto fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
        file.read(reinterpret_cast<char*>(fileData.data()), fileSize);
        file.close();

        if (fileData.size() < sizeof(WAVHeader))
            return false;

        // Parse RIFF header
        auto* header = reinterpret_cast<const WAVHeader*>(fileData.data());
        if (std::memcmp(header->riffId, "RIFF", 4) != 0 || std::memcmp(header->waveId, "WAVE", 4) != 0)
        {
            fprintf(stderr, "[OpenAL] Not a valid WAV file: %s\n", filename.c_str());
            return false;
        }

        // Find fmt chunk
        const WAVFmtChunk* fmt = nullptr;
        const uint8_t* dataChunk = nullptr;
        uint32_t dataSize = 0;

        size_t offset = sizeof(WAVHeader);
        while (offset + sizeof(WAVChunkHeader) <= fileData.size())
        {
            auto* chunk = reinterpret_cast<const WAVChunkHeader*>(fileData.data() + offset);

            if (std::memcmp(chunk->id, "fmt ", 4) == 0)
            {
                fmt = reinterpret_cast<const WAVFmtChunk*>(fileData.data() + offset + sizeof(WAVChunkHeader));
            }
            else if (std::memcmp(chunk->id, "data", 4) == 0)
            {
                dataChunk = fileData.data() + offset + sizeof(WAVChunkHeader);
                dataSize = chunk->size;
            }

            size_t chunkTotalSize = sizeof(WAVChunkHeader) + chunk->size;
            if (chunkTotalSize < chunk->size || offset + chunkTotalSize < offset)
                break; // overflow protection
            offset += chunkTotalSize;
            if (offset % 2 != 0)
                offset++; // WAV chunks are 2-byte aligned
        }

        if (!fmt || !dataChunk || dataSize == 0)
        {
            fprintf(stderr, "[OpenAL] Invalid WAV structure: %s\n", filename.c_str());
            return false;
        }

        // Determine OpenAL format
        ALenum alFormat;
        if (fmt->numChannels == 1 && fmt->bitsPerSample == 8)
            alFormat = AL_FORMAT_MONO8;
        else if (fmt->numChannels == 1 && fmt->bitsPerSample == 16)
            alFormat = AL_FORMAT_MONO16;
        else if (fmt->numChannels == 2 && fmt->bitsPerSample == 8)
            alFormat = AL_FORMAT_STEREO8;
        else if (fmt->numChannels == 2 && fmt->bitsPerSample == 16)
            alFormat = AL_FORMAT_STEREO16;
        else
        {
            fprintf(stderr, "[OpenAL] Unsupported format: %uch, %ubit\n", fmt->numChannels, fmt->bitsPerSample);
            return false;
        }

        // Create OpenAL buffer
        ALuint buffer;
        alGenBuffers(1, &buffer);
        alBufferData(buffer, alFormat, dataChunk, static_cast<ALsizei>(dataSize),
                     static_cast<ALsizei>(fmt->sampleRate));

        if (alGetError() != AL_NO_ERROR)
        {
            fprintf(stderr, "[OpenAL] Failed to create buffer for: %s\n", filename.c_str());
            alDeleteBuffers(1, &buffer);
            return false;
        }

        outBuffer.alBuffer = buffer;
        outBuffer.sampleRate = fmt->sampleRate;
        outBuffer.channels = fmt->numChannels;
        outBuffer.bitsPerSample = fmt->bitsPerSample;
        outBuffer.duration =
            static_cast<float>(dataSize) / static_cast<float>(fmt->byteRate > 0 ? fmt->byteRate : fmt->sampleRate);

        return true;
#else
        (void)filename;
        (void)outBuffer;
        return false;
#endif
    }

    OpenALSource* OpenALAudioEngine::GetAvailableSource()
    {
        if (m_availableSources.empty())
            return nullptr;
        auto* src = m_availableSources.back();
        m_availableSources.pop_back();
        return src;
    }

    void OpenALAudioEngine::ReturnSource(OpenALSource* source)
    {
        if (!source)
            return;
        source->IsPlaying = false;
        source->SoundName.clear();
        m_availableSources.push_back(source);
    }

    void OpenALAudioEngine::UpdateSources()
    {
#ifdef SPARK_OPENAL_AVAILABLE
        for (auto& src : m_sources)
        {
            if (!src->IsPlaying)
                continue;

            ALint state;
            alGetSourcei(src->alSource, AL_SOURCE_STATE, &state);
            if (state == AL_STOPPED)
            {
                alSourcei(src->alSource, AL_BUFFER, 0);
                ReturnSource(src.get());
            }
        }
#endif
    }

} // namespace Spark::Audio

#endif // !SPARK_PLATFORM_WINDOWS
