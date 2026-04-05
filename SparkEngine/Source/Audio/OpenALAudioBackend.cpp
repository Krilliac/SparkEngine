/**
 * @file OpenALAudioBackend.cpp
 * @brief IAudioBackend adapter delegating to the OpenAL Soft engine
 */

#include "OpenALAudioBackend.h"
#include "Core/Platform.h"

#if !defined(SPARK_PLATFORM_WINDOWS)

namespace Spark::Audio
{

    OpenALAudioBackend::OpenALAudioBackend() : m_engine(std::make_unique<OpenALAudioEngine>()) {}

    OpenALAudioBackend::~OpenALAudioBackend()
    {
        if (m_engine && m_engine->IsInitialized())
            m_engine->Shutdown();
    }

    bool OpenALAudioBackend::Initialize(size_t maxSources)
    {
        return m_engine && m_engine->Initialize(maxSources);
    }

    void OpenALAudioBackend::Shutdown()
    {
        if (m_engine)
            m_engine->Shutdown();
    }

    void OpenALAudioBackend::Update(float deltaTime)
    {
        if (m_engine)
            m_engine->Update(deltaTime);
    }

    bool OpenALAudioBackend::LoadSound(const std::string& name, const std::string& filepath)
    {
        return m_engine && m_engine->LoadSound(name, filepath);
    }

    void OpenALAudioBackend::UnloadSound(const std::string& name)
    {
        if (m_engine)
            m_engine->UnloadSound(name);
    }

    uint32_t OpenALAudioBackend::PlaySound(const std::string& name, float volume, float pitch, bool loop)
    {
        if (!m_engine)
            return 0;
        auto* source = m_engine->PlaySound(name, volume, pitch, loop);
        return source ? source->SourceID : 0;
    }

    uint32_t OpenALAudioBackend::PlaySound3D(const std::string& name, float x, float y, float z, float volume,
                                             float pitch, bool loop)
    {
        if (!m_engine)
            return 0;
        Float3 pos{x, y, z};
        auto* source = m_engine->PlaySound3D(name, pos, volume, pitch, loop);
        return source ? source->SourceID : 0;
    }

    void OpenALAudioBackend::StopSound(uint32_t handle)
    {
        // OpenALAudioEngine uses pointer-based stop; handle lookup not directly supported
        (void)handle;
    }

    void OpenALAudioBackend::StopAllSounds()
    {
        if (m_engine)
            m_engine->StopAllSounds();
    }

    void OpenALAudioBackend::PauseAllSounds()
    {
        if (m_engine)
            m_engine->PauseAllSounds();
    }

    void OpenALAudioBackend::ResumeAllSounds()
    {
        if (m_engine)
            m_engine->ResumeAllSounds();
    }

    void OpenALAudioBackend::SetMasterVolume(float volume)
    {
        if (m_engine)
            m_engine->SetMasterVolume(volume);
    }

    void OpenALAudioBackend::SetSFXVolume(float volume)
    {
        if (m_engine)
            m_engine->SetSFXVolume(volume);
    }

    void OpenALAudioBackend::SetMusicVolume(float volume)
    {
        if (m_engine)
            m_engine->SetMusicVolume(volume);
    }

    float OpenALAudioBackend::GetMasterVolume() const
    {
        return m_engine ? m_engine->GetMasterVolume() : 1.0f;
    }

    float OpenALAudioBackend::GetSFXVolume() const
    {
        return m_engine ? m_engine->GetSFXVolume() : 1.0f;
    }

    float OpenALAudioBackend::GetMusicVolume() const
    {
        return m_engine ? m_engine->GetMusicVolume() : 1.0f;
    }

    void OpenALAudioBackend::SetListenerPosition(float x, float y, float z)
    {
        if (m_engine)
            m_engine->SetListenerPosition({x, y, z});
    }

    void OpenALAudioBackend::SetListenerOrientation(float fx, float fy, float fz, float ux, float uy, float uz)
    {
        if (m_engine)
            m_engine->SetListenerOrientation({fx, fy, fz}, {ux, uy, uz});
    }

    void OpenALAudioBackend::SetListenerVelocity(float vx, float vy, float vz)
    {
        if (m_engine)
            m_engine->SetListenerVelocity({vx, vy, vz});
    }

    void OpenALAudioBackend::Set3DEnabled(bool enabled)
    {
        if (m_engine)
            m_engine->Set3DEnabled(enabled);
    }

    bool OpenALAudioBackend::IsAvailable() const
    {
        return m_engine && m_engine->IsInitialized();
    }

} // namespace Spark::Audio

#endif // !SPARK_PLATFORM_WINDOWS
