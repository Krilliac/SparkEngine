#include "Core/Platform.h"
// AudioEngine.cpp
#include "AudioEngine.h"
#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "../Utils/SparkConsole.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>

using namespace DirectX;
AudioEngine::AudioEngine()
    : m_xAudio2(nullptr), m_masterVoice(nullptr), m_masterVolume(1.0f), m_sfxVolume(1.0f), m_musicVolume(1.0f),
      m_maxSources(32), m_nextSourceID(1), m_listenerPosition(0.0f, 0.0f, 0.0f), m_listenerVelocity(0.0f, 0.0f, 0.0f),
      m_listenerForward(0.0f, 0.0f, 1.0f), m_listenerUp(0.0f, 1.0f, 0.0f), m_dopplerScale(1.0f), m_distanceScale(1.0f),
      m_3DEnabled(true)
{
    // Initialize console-controlled settings to defaults
    m_settings.masterVolume = 1.0f;
    m_settings.sfxVolume = 1.0f;
    m_settings.musicVolume = 1.0f;
    m_settings.dopplerScale = 1.0f;
    m_settings.distanceScale = 1.0f;
    m_settings.enable3D = true;
    m_settings.enableReverb = false;
    m_settings.enableEAX = false;
    m_settings.maxSources = 32;
    m_settings.listenerPosition = XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_settings.listenerVelocity = XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_settings.listenerForward = XMFLOAT3(0.0f, 0.0f, 1.0f);
    m_settings.listenerUp = XMFLOAT3(0.0f, 1.0f, 0.0f);

    Spark::SimpleConsole::GetInstance().Log("AudioEngine constructed with console integration.", "INFO");
}

AudioEngine::~AudioEngine()
{
    Spark::SimpleConsole::GetInstance().Log("AudioEngine destructor called.", "INFO");
    Shutdown();
}

HRESULT AudioEngine::Initialize(size_t maxSources)
{
    SPARK_LOG_INFO("Audio", "AudioEngine::Initialize -- maxSources=%zu", maxSources);
    ASSERT_MSG(maxSources > 0, "AudioEngine maxSources must be positive");
    m_maxSources = maxSources;
    m_settings.maxSources = static_cast<int>(maxSources);

    HRESULT hr = XAudio2Create(&m_xAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        SPARK_LOG_FATAL("Audio",
                        "XAudio2Create failed HR=0x%08lX -- "
                        "Check audio drivers and Windows Audio Service",
                        static_cast<long>(hr));
        return hr;
    }

    hr = m_xAudio2->CreateMasteringVoice(&m_masterVoice);
    if (FAILED(hr))
    {
        SPARK_LOG_FATAL("Audio",
                        "CreateMasteringVoice failed HR=0x%08lX -- "
                        "No audio output device available?",
                        static_cast<long>(hr));
        return hr;
    }

    m_audioSources.clear();
    m_availableSources.clear();
    m_audioSources.reserve(m_maxSources);
    m_availableSources.reserve(m_maxSources);

    for (size_t i = 0; i < m_maxSources; ++i)
    {
        auto source = std::make_unique<AudioSource>();
        ASSERT_NOT_NULL(source.get());
        source->SourceID = m_nextSourceID++;
        m_availableSources.push_back(source.get());
        m_audioSources.push_back(std::move(source));
    }

    Spark::SimpleConsole::GetInstance().Log(
        "AudioEngine initialization complete with console integration - audio ready.", "SUCCESS");
    return S_OK;
}

void AudioEngine::Update(float deltaTime)
{
    static bool firstFrame = true;
    if (firstFrame)
    {
        Spark::SimpleConsole::GetInstance().Log("AudioEngine::Update - First frame started with console integration",
                                                "INFO");
        firstFrame = false;
    }

    UpdateSources();

    if (m_3DEnabled)
    {
        Update3DAudio();
    }
}

void AudioEngine::Shutdown()
{
    Spark::SimpleConsole::GetInstance().Log("AudioEngine::Shutdown called.", "INFO");
    StopAllSounds();
    m_soundEffects.clear();
    m_audioSources.clear();
    m_availableSources.clear();

    if (m_masterVoice)
    {
        m_masterVoice->DestroyVoice();
        m_masterVoice = nullptr;
    }
    if (m_xAudio2)
    {
        m_xAudio2->Release();
        m_xAudio2 = nullptr;
    }
    Spark::SimpleConsole::GetInstance().Log("AudioEngine shutdown complete.", "INFO");
}

HRESULT AudioEngine::LoadSound(const std::string& name, const std::wstring& filename)
{
    ASSERT_MSG(!name.empty(), "Sound name must be non-empty");
    auto sound = std::make_unique<SoundEffect>();
    ASSERT_NOT_NULL(sound.get());

    HRESULT hr = sound->LoadFromFile(filename);
    ASSERT_MSG(SUCCEEDED(hr), "SoundEffect::LoadFromFile failed");
    if (FAILED(hr))
    {
        Spark::SimpleConsole::GetInstance().Log("Failed to load sound '" + name + "' from file", "ERROR");
        return hr;
    }

    m_soundEffects[name] = std::move(sound);
    Spark::SimpleConsole::GetInstance().Log("Sound '" + name + "' loaded successfully", "SUCCESS");
    return S_OK;
}

void AudioEngine::UnloadSound(const std::string& name)
{
    auto it = m_soundEffects.find(name);
    if (it != m_soundEffects.end())
    {
        for (auto& s : m_audioSources)
        {
            if (s->Sound == it->second.get() && s->IsPlaying)
                StopSound(s.get());
        }
        m_soundEffects.erase(it);
        Spark::SimpleConsole::GetInstance().Log("Sound '" + name + "' unloaded", "INFO");
    }
}

SoundEffect* AudioEngine::GetSound(const std::string& name)
{
    auto it = m_soundEffects.find(name);
    return it != m_soundEffects.end() ? it->second.get() : nullptr;
}

AudioSource* AudioEngine::PlaySound(const std::string& name, float volume, float pitch, bool loop)
{
    ASSERT_MSG(!name.empty(), "Sound name must be non-empty");
    SoundEffect* sound = GetSound(name);
    if (!sound)
    {
        Spark::SimpleConsole::GetInstance().Log("Sound '" + name + "' not found", "ERROR");
        return nullptr;
    }

    AudioSource* src = GetAvailableSource();
    if (!src)
    {
        Spark::SimpleConsole::GetInstance().Log("No available audio sources", "WARNING");
        return nullptr;
    }

    if (!src->Voice)
    {
        HRESULT hr = CreateSourceVoice(sound->GetFormat(), &src->Voice);
        ASSERT_MSG(SUCCEEDED(hr), "CreateSourceVoice failed");
        if (FAILED(hr))
            return nullptr;
    }

    if (volume < 0.0f || volume > 1.0f)
    {
        SPARK_LOG_WARN("Audio", "PlaySound '%s': volume %.2f clamped to [0,1]", name.c_str(), volume);
        volume = std::clamp(volume, 0.0f, 1.0f);
    }
    if (pitch <= 0.0f)
    {
        SPARK_LOG_WARN("Audio", "PlaySound '%s': pitch %.2f invalid, reset to 1.0", name.c_str(), pitch);
        pitch = 1.0f;
    }

    src->Sound = sound;
    src->Volume = volume * m_sfxVolume * m_masterVolume;
    src->Pitch = pitch;
    src->IsLooping = loop;
    src->Is3D = false;
    src->IsPlaying = true;

    src->Voice->SetVolume(src->Volume);
    src->Voice->SetFrequencyRatio(src->Pitch);

    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = sound->GetDataSize();
    ASSERT_MSG(buffer.AudioBytes > 0, "Empty audio data");
    buffer.pAudioData = sound->GetData();
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (loop)
        buffer.LoopCount = XAUDIO2_LOOP_INFINITE;

    HRESULT hr = src->Voice->SubmitSourceBuffer(&buffer);
    ASSERT_MSG(SUCCEEDED(hr), "SubmitSourceBuffer failed");
    if (FAILED(hr))
        return nullptr;

    src->Voice->Start();
    return src;
}

AudioSource* AudioEngine::PlaySound3D(const std::string& name, const XMFLOAT3& position, float volume, float pitch,
                                      bool loop)
{
    AudioSource* src = PlaySound(name, volume, pitch, loop);
    if (src)
    {
        src->Is3D = true;
        src->Position = position;

        // Apply 3D audio calculations immediately
        if (m_3DEnabled)
        {
            Apply3DAudioToSource(src);
        }
    }
    return src;
}

void AudioEngine::StopSound(AudioSource* source)
{
    ASSERT_NOT_NULL(source);
    if (source->IsPlaying && source->Voice)
    {
        source->Voice->Stop();
        source->Voice->FlushSourceBuffers();
        source->IsPlaying = false;
        ReturnSource(source);
    }
}

void AudioEngine::StopAllSounds()
{
    for (auto& s : m_audioSources)
        if (s->IsPlaying)
            StopSound(s.get());
}

void AudioEngine::PauseAllSounds()
{
    for (auto& s : m_audioSources)
        if (s->IsPlaying && s->Voice)
            s->Voice->Stop();
}

void AudioEngine::ResumeAllSounds()
{
    for (auto& s : m_audioSources)
        if (s->IsPlaying && s->Voice)
            s->Voice->Start();
}

void AudioEngine::SetMasterVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    m_settings.masterVolume = m_masterVolume;

    if (m_masterVoice)
    {
        m_masterVoice->SetVolume(m_masterVolume);
    }

    NotifyStateChange();
}

void AudioEngine::SetSFXVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_sfxVolume = std::clamp(volume, 0.0f, 1.0f);
    m_settings.sfxVolume = m_sfxVolume;
    NotifyStateChange();
}

void AudioEngine::SetMusicVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_musicVolume = std::clamp(volume, 0.0f, 1.0f);
    m_settings.musicVolume = m_musicVolume;
    NotifyStateChange();
}

size_t AudioEngine::GetActiveSourceCount() const
{
    size_t count = 0;
    for (auto& s : m_audioSources)
        if (s->IsPlaying)
            ++count;
    return count;
}

// ============================================================================
// CONSOLE INTEGRATION IMPLEMENTATIONS
// ============================================================================

void AudioEngine::Console_SetMasterVolume(float volume)
{
    if (volume >= 0.0f && volume <= 1.0f)
    {
        SetMasterVolume(volume);
        Spark::SimpleConsole::GetInstance().Log("Master volume set to " + std::to_string(volume) + " via console",
                                                "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid master volume. Must be between 0.0 and 1.0", "ERROR");
    }
}

void AudioEngine::Console_SetSFXVolume(float volume)
{
    if (volume >= 0.0f && volume <= 1.0f)
    {
        SetSFXVolume(volume);
        Spark::SimpleConsole::GetInstance().Log("SFX volume set to " + std::to_string(volume) + " via console",
                                                "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid SFX volume. Must be between 0.0 and 1.0", "ERROR");
    }
}

void AudioEngine::Console_SetMusicVolume(float volume)
{
    if (volume >= 0.0f && volume <= 1.0f)
    {
        SetMusicVolume(volume);
        Spark::SimpleConsole::GetInstance().Log("Music volume set to " + std::to_string(volume) + " via console",
                                                "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid music volume. Must be between 0.0 and 1.0", "ERROR");
    }
}

void AudioEngine::Console_SetListenerPosition(float x, float y, float z)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_listenerPosition = XMFLOAT3(x, y, z);
    m_settings.listenerPosition = m_listenerPosition;
    NotifyStateChange();

    std::stringstream ss;
    ss << "3D listener position set to (" << x << ", " << y << ", " << z << ") via console";
    Spark::SimpleConsole::GetInstance().Log(ss.str(), "SUCCESS");
}

void AudioEngine::Console_SetListenerOrientation(float forwardX, float forwardY, float forwardZ, float upX, float upY,
                                                 float upZ)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_listenerForward = XMFLOAT3(forwardX, forwardY, forwardZ);
    m_listenerUp = XMFLOAT3(upX, upY, upZ);
    m_settings.listenerForward = m_listenerForward;
    m_settings.listenerUp = m_listenerUp;
    NotifyStateChange();

    std::stringstream ss;
    ss << "3D listener orientation set - Forward: (" << forwardX << ", " << forwardY << ", " << forwardZ << "), Up: ("
       << upX << ", " << upY << ", " << upZ << ") via console";
    Spark::SimpleConsole::GetInstance().Log(ss.str(), "SUCCESS");
}

void AudioEngine::Console_SetDopplerScale(float scale)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    if (scale >= 0.0f && scale <= 2.0f)
    {
        m_dopplerScale = scale;
        m_settings.dopplerScale = scale;
        NotifyStateChange();
        Spark::SimpleConsole::GetInstance().Log("Doppler scale set to " + std::to_string(scale) + " via console",
                                                "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid Doppler scale. Must be between 0.0 and 2.0", "ERROR");
    }
}

void AudioEngine::Console_SetDistanceScale(float scale)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    if (scale >= 0.1f && scale <= 10.0f)
    {
        m_distanceScale = scale;
        m_settings.distanceScale = scale;
        NotifyStateChange();
        Spark::SimpleConsole::GetInstance().Log("Distance scale set to " + std::to_string(scale) + " via console",
                                                "SUCCESS");
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Invalid distance scale. Must be between 0.1 and 10.0", "ERROR");
    }
}

void AudioEngine::Console_Set3DAudio(bool enabled)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_3DEnabled = enabled;
    m_settings.enable3D = enabled;
    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log(
        "3D audio " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

uint32_t AudioEngine::Console_PlayTestSound(const std::string& soundName, bool is3D)
{
    SoundEffect* sound = GetSound(soundName);
    if (!sound)
    {
        // Try to create a procedural test sound
        if (soundName == "test_beep")
        {
            auto testSound = SoundEffectFactory::CreateBeep(440.0f, 0.5f);
            if (testSound)
            {
                m_soundEffects["test_beep"] = std::move(testSound);
                sound = m_soundEffects["test_beep"].get();
                Spark::SimpleConsole::GetInstance().Log("Created procedural test beep sound", "INFO");
            }
        }
        else if (soundName == "test_gunshot")
        {
            auto testSound = SoundEffectFactory::CreateGunshot();
            if (testSound)
            {
                m_soundEffects["test_gunshot"] = std::move(testSound);
                sound = m_soundEffects["test_gunshot"].get();
                Spark::SimpleConsole::GetInstance().Log("Created procedural gunshot sound", "INFO");
            }
        }
        else if (soundName == "test_explosion")
        {
            auto testSound = SoundEffectFactory::CreateExplosion();
            if (testSound)
            {
                m_soundEffects["test_explosion"] = std::move(testSound);
                sound = m_soundEffects["test_explosion"].get();
                Spark::SimpleConsole::GetInstance().Log("Created procedural explosion sound", "INFO");
            }
        }
        else if (soundName == "test_footstep")
        {
            auto testSound = SoundEffectFactory::CreateFootstep();
            if (testSound)
            {
                m_soundEffects["test_footstep"] = std::move(testSound);
                sound = m_soundEffects["test_footstep"].get();
                Spark::SimpleConsole::GetInstance().Log("Created procedural footstep sound", "INFO");
            }
        }
        else if (soundName == "test_pickup")
        {
            auto testSound = SoundEffectFactory::CreatePickup();
            if (testSound)
            {
                m_soundEffects["test_pickup"] = std::move(testSound);
                sound = m_soundEffects["test_pickup"].get();
                Spark::SimpleConsole::GetInstance().Log("Created procedural pickup sound", "INFO");
            }
        }
        else if (soundName == "test_noise")
        {
            auto testSound = SoundEffectFactory::CreateNoise(1.0f);
            if (testSound)
            {
                m_soundEffects["test_noise"] = std::move(testSound);
                sound = m_soundEffects["test_noise"].get();
                Spark::SimpleConsole::GetInstance().Log("Created procedural noise sound", "INFO");
            }
        }
    }

    if (!sound)
    {
        Spark::SimpleConsole::GetInstance().Log(
            "Sound '" + soundName + "' not found and could not create procedural version", "ERROR");
        return 0;
    }

    AudioSource* source = nullptr;
    if (is3D)
    {
        // Play at a test position in front of the listener
        XMFLOAT3 testPos(m_listenerPosition.x, m_listenerPosition.y, m_listenerPosition.z + 5.0f);
        source = PlaySound3D(soundName, testPos, 1.0f, 1.0f, false);
    }
    else
    {
        source = PlaySound(soundName, 1.0f, 1.0f, false);
    }

    if (source)
    {
        std::string modeStr = is3D ? "3D" : "2D";
        Spark::SimpleConsole::GetInstance().Log("Playing test sound '" + soundName + "' in " + modeStr +
                                                    " mode (ID: " + std::to_string(source->SourceID) + ")",
                                                "SUCCESS");
        return source->SourceID;
    }
    else
    {
        Spark::SimpleConsole::GetInstance().Log("Failed to play test sound '" + soundName + "'", "ERROR");
        return 0;
    }
}

void AudioEngine::Console_StopSound(uint32_t sourceID)
{
    for (auto& source : m_audioSources)
    {
        if (source->SourceID == sourceID && source->IsPlaying)
        {
            StopSound(source.get());
            Spark::SimpleConsole::GetInstance().Log(
                "Stopped sound source ID " + std::to_string(sourceID) + " via console", "SUCCESS");
            return;
        }
    }
    Spark::SimpleConsole::GetInstance().Log("Sound source ID " + std::to_string(sourceID) + " not found or not playing",
                                            "ERROR");
}

void AudioEngine::Console_StopAllSounds()
{
    size_t stoppedCount = GetActiveSourceCount();
    StopAllSounds();
    Spark::SimpleConsole::GetInstance().Log("Stopped " + std::to_string(stoppedCount) + " playing sounds via console",
                                            "SUCCESS");
}

std::string AudioEngine::Console_ListSounds() const
{
    std::stringstream ss;
    ss << "Loaded Sounds (" << m_soundEffects.size() << " total):\n";
    ss << "==========================================\n";

    for (const auto& pair : m_soundEffects)
    {
        const auto& sound = pair.second;
        ss << "  " << std::left << std::setw(20) << pair.first << " - " << std::fixed << std::setprecision(2)
           << sound->GetDuration() << "s, " << sound->GetSampleRate() << "Hz, " << sound->GetChannels() << "ch, "
           << sound->GetBitsPerSample() << "bit\n";
    }

    if (m_soundEffects.empty())
    {
        ss << "  No sounds currently loaded\n";
        ss << "\nAvailable procedural test sounds:\n";
        ss << "  test_beep, test_gunshot, test_explosion, test_footstep, test_pickup, test_noise\n";
        ss << "  Use 'audio_test <soundname>' to create and play them\n";
    }

    return ss.str();
}

AudioEngine::AudioMetrics AudioEngine::Console_GetMetrics() const
{
    return GetMetricsThreadSafe();
}

AudioEngine::AudioSettings AudioEngine::Console_GetSettings() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_settings;
}

void AudioEngine::Console_ApplySettings(const AudioSettings& settings)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_settings = settings;

    // Apply settings to actual audio engine
    m_masterVolume = settings.masterVolume;
    m_sfxVolume = settings.sfxVolume;
    m_musicVolume = settings.musicVolume;
    m_dopplerScale = settings.dopplerScale;
    m_distanceScale = settings.distanceScale;
    m_3DEnabled = settings.enable3D;
    m_listenerPosition = settings.listenerPosition;
    m_listenerVelocity = settings.listenerVelocity;
    m_listenerForward = settings.listenerForward;
    m_listenerUp = settings.listenerUp;

    if (m_masterVoice)
    {
        m_masterVoice->SetVolume(m_masterVolume);
    }

    NotifyStateChange();
    Spark::SimpleConsole::GetInstance().Log("Audio settings applied via console", "SUCCESS");
}

void AudioEngine::Console_ResetToDefaults()
{
    Console_SetMasterVolume(1.0f);
    Console_SetSFXVolume(1.0f);
    Console_SetMusicVolume(1.0f);
    Console_SetDopplerScale(1.0f);
    Console_SetDistanceScale(1.0f);
    Console_Set3DAudio(true);
    Console_SetListenerPosition(0.0f, 0.0f, 0.0f);
    Console_SetListenerOrientation(0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
    Spark::SimpleConsole::GetInstance().Log("Audio settings reset to defaults via console", "SUCCESS");
}

void AudioEngine::Console_RegisterStateCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    m_stateCallback = callback;
    Spark::SimpleConsole::GetInstance().Log("Audio state callback registered", "INFO");
}

void AudioEngine::Console_RefreshAudio()
{
    Spark::SimpleConsole::GetInstance().Log("Audio system refresh requested via console", "INFO");

    // Force update all 3D audio sources
    if (m_3DEnabled)
    {
        Update3DAudio();
    }

    // Update volume for all active sources
    for (auto& source : m_audioSources)
    {
        if (source->IsPlaying && source->Voice)
        {
            float adjustedVolume = source->Volume * m_sfxVolume * m_masterVolume;
            source->Voice->SetVolume(adjustedVolume);
        }
    }

    Spark::SimpleConsole::GetInstance().Log("Audio system refresh complete", "SUCCESS");
}

std::string AudioEngine::Console_GetSourceInfo(uint32_t sourceID) const
{
    for (const auto& source : m_audioSources)
    {
        if (source->SourceID == sourceID)
        {
            std::stringstream ss;
            ss << "Audio Source ID " << sourceID << ":\n";
            ss << "==========================================\n";
            ss << "Status:           " << (source->IsPlaying ? "PLAYING" : "STOPPED") << "\n";
            ss << "3D Audio:         " << (source->Is3D ? "YES" : "NO") << "\n";
            ss << "Looping:          " << (source->IsLooping ? "YES" : "NO") << "\n";
            ss << "Volume:           " << std::fixed << std::setprecision(2) << source->Volume << "\n";
            ss << "Pitch:            " << std::setprecision(2) << source->Pitch << "\n";

            if (source->Is3D)
            {
                ss << "Position:         (" << std::setprecision(2) << source->Position.x << ", " << source->Position.y
                   << ", " << source->Position.z << ")\n";
                ss << "Velocity:         (" << std::setprecision(2) << source->Velocity.x << ", " << source->Velocity.y
                   << ", " << source->Velocity.z << ")\n";
            }

            if (source->Sound)
            {
                ss << "Sound Duration:   " << std::setprecision(2) << source->Sound->GetDuration() << "s\n";
                ss << "Sample Rate:      " << source->Sound->GetSampleRate() << "Hz\n";
                ss << "Channels:         " << source->Sound->GetChannels() << "\n";
                ss << "Bits Per Sample:  " << source->Sound->GetBitsPerSample() << "\n";
            }

            return ss.str();
        }
    }
    return "Audio source ID " + std::to_string(sourceID) + " not found";
}

// ============================================================================
// PRIVATE HELPER METHODS
// ============================================================================

AudioSource* AudioEngine::GetAvailableSource()
{
    if (m_availableSources.empty())
        return nullptr;
    AudioSource* src = m_availableSources.back();
    m_availableSources.pop_back();
    return src;
}

void AudioEngine::ReturnSource(AudioSource* source)
{
    ASSERT_NOT_NULL(source);
    source->IsPlaying = false;
    source->Sound = nullptr;
    m_availableSources.push_back(source);
}

void AudioEngine::UpdateSources()
{
    for (auto& s : m_audioSources)
    {
        if (s->IsPlaying && s->Voice)
        {
            XAUDIO2_VOICE_STATE state;
            s->Voice->GetState(&state);
            if (state.BuffersQueued == 0)
                StopSound(s.get());
        }
    }
}

void AudioEngine::Update3DAudio()
{
    for (auto& source : m_audioSources)
    {
        if (source->IsPlaying && source->Is3D)
        {
            Apply3DAudioToSource(source.get());
        }
    }
}

void AudioEngine::Apply3DAudioToSource(AudioSource* source)
{
    if (!source || !source->Voice || !source->Is3D)
        return;

    // Calculate relative position from listener to source
    float dx = source->Position.x - m_listenerPosition.x;
    float dy = source->Position.y - m_listenerPosition.y;
    float dz = source->Position.z - m_listenerPosition.z;
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);

    // Apply distance attenuation
    float attenuation = 1.0f / (1.0f + distance * m_distanceScale * 0.1f);
    attenuation = std::clamp(attenuation, 0.0f, 1.0f);

    // Apply volume with 3D attenuation
    float finalVolume = source->Volume * attenuation;
    source->Voice->SetVolume(finalVolume);

    // Stereo panning based on relative position to listener
    // Calculate the cross product of listener forward and direction to source
    // to determine left/right placement
    if (distance > 0.001f)
    {
        // Normalize direction to source
        float invDist = 1.0f / distance;
        float dirX = dx * invDist;
        float dirZ = dz * invDist;

        // Cross product of listener forward (XZ plane) with source direction gives left/right
        // Positive = right, Negative = left
        float pan = m_listenerForward.x * dirZ - m_listenerForward.z * dirX;
        pan = std::clamp(pan, -1.0f, 1.0f);

        // Convert pan to left/right channel volumes using equal-power panning
        // pan: -1 = full left, 0 = center, +1 = full right
        float angle = (pan + 1.0f) * 0.5f * 3.14159265f * 0.5f;
        float leftGain = cosf(angle);
        float rightGain = sinf(angle);

        // Apply stereo output matrix: [leftToLeft, rightToLeft, leftToRight, rightToRight]
        // For mono source -> stereo output: [left, right]
        float outputMatrix[2] = {leftGain, rightGain};

        XAUDIO2_VOICE_DETAILS voiceDetails;
        source->Voice->GetVoiceDetails(&voiceDetails);

        XAUDIO2_VOICE_DETAILS masterDetails;
        m_masterVoice->GetVoiceDetails(&masterDetails);

        // SetOutputMatrix: srcChannels * dstChannels matrix
        source->Voice->SetOutputMatrix(m_masterVoice, voiceDetails.InputChannels, masterDetails.InputChannels,
                                       outputMatrix);
    }

    // Doppler effect using relative velocity between source and listener
    if (m_dopplerScale > 0.0f && distance > 0.001f)
    {
        const float speedOfSound = 343.0f; // m/s

        // Calculate relative velocity along the line between source and listener
        float invDist = 1.0f / distance;
        float dirToListenerX = -dx * invDist;
        float dirToListenerY = -dy * invDist;
        float dirToListenerZ = -dz * invDist;

        // Project source velocity onto direction toward listener
        float sourceApproachSpeed = source->Velocity.x * dirToListenerX + source->Velocity.y * dirToListenerY +
                                    source->Velocity.z * dirToListenerZ;

        // Project listener velocity onto direction toward source
        float listenerApproachSpeed = m_listenerVelocity.x * (-dirToListenerX) +
                                      m_listenerVelocity.y * (-dirToListenerY) +
                                      m_listenerVelocity.z * (-dirToListenerZ);

        // Doppler ratio: f' = f * (v + vL) / (v + vS)
        // where v = speed of sound, vL = listener approach speed, vS = source approach speed
        float numerator = speedOfSound + listenerApproachSpeed * m_dopplerScale;
        float denominator = speedOfSound + sourceApproachSpeed * m_dopplerScale;

        // Clamp to prevent extreme pitch shifts or division issues
        if (denominator > 0.01f)
        {
            float dopplerRatio = std::clamp(numerator / denominator, 0.5f, 2.0f);
            source->Voice->SetFrequencyRatio(source->Pitch * dopplerRatio);
        }
    }
}

HRESULT AudioEngine::CreateSourceVoice(const WAVEFORMATEX& format, IXAudio2SourceVoice** voice)
{
    ASSERT_MSG(m_xAudio2 != nullptr, "XAudio2 not initialized");
    return m_xAudio2->CreateSourceVoice(voice, &format);
}

void AudioEngine::NotifyStateChange()
{
    if (m_stateCallback)
    {
        m_stateCallback();
    }
}

AudioEngine::AudioMetrics AudioEngine::GetMetricsThreadSafe() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);

    AudioMetrics metrics;
    metrics.activeSources = GetActiveSourceCount();
    metrics.totalSources = m_audioSources.size();
    metrics.loadedSounds = m_soundEffects.size();
    metrics.masterVolume = m_masterVolume;
    metrics.sfxVolume = m_sfxVolume;
    metrics.musicVolume = m_musicVolume;
    metrics.cpuUsage = 0.0f; // Would need platform-specific implementation
    metrics.memoryUsage = 0; // Would need detailed memory tracking
    metrics.is3DEnabled = m_3DEnabled;
    metrics.listenerPosition = m_listenerPosition;
    metrics.listenerVelocity = m_listenerVelocity;
    metrics.dopplerScale = m_dopplerScale;
    metrics.distanceScale = m_distanceScale;

    return metrics;
}
