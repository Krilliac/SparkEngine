#include "Core/Platform.h"
#include "SoundEffect.h"
#include "Utils/Assert.h"
#include <fstream>
#include <cmath>
#include <random>
#include <filesystem>
#include <iostream>

//------------------------------------------------------------------------------
//  SoundEffect implementation
//------------------------------------------------------------------------------
SoundEffect::SoundEffect() {
    std::wcout << L"[INFO] SoundEffect constructed." << std::endl;
}
SoundEffect::~SoundEffect() {
    std::wcout << L"[INFO] SoundEffect destructor called." << std::endl;
}

HRESULT SoundEffect::LoadFromFile(const std::wstring& filename)
{
    ASSERT_ALWAYS_MSG(!filename.empty(), "SoundEffect::LoadFromFile - empty filename");

    std::wcout << L"[OPERATION] SoundEffect::LoadFromFile called. filename=" << filename << std::endl;

#ifdef SPARK_PLATFORM_WINDOWS
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
#else
    // Linux: convert wstring to narrow string for ifstream
    std::string narrowFilename(filename.begin(), filename.end());
    std::ifstream file(narrowFilename, std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open())
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

    std::streamsize size = file.tellg();
    ASSERT_ALWAYS_MSG(size > 0, "Zero-length WAV file");
    file.seekg(0, std::ios::beg);

    std::vector<BYTE> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
        return E_FAIL;

    return ParseWAVFile(buffer.data(), static_cast<DWORD>(size));
}

HRESULT SoundEffect::LoadFromMemory(const BYTE* data, DWORD dataSize)
{
    ASSERT_ALWAYS(data && dataSize);
    return ParseWAVFile(data, dataSize);
}

void SoundEffect::Unload()
{
    m_audioData.clear();
    m_audioDataSize = 0;
    ZeroMemory(&m_format, sizeof(m_format));
    std::wcout << L"[INFO] SoundEffect resources unloaded." << std::endl;
}

float SoundEffect::GetDuration() const
{
    return (m_format.nAvgBytesPerSec == 0)
        ? 0.f
        : static_cast<float>(m_audioDataSize) / m_format.nAvgBytesPerSec;
}

// ---------------------------------------------------------------------------
//  Private helpers
// ---------------------------------------------------------------------------
HRESULT SoundEffect::ParseWAVFile(const BYTE* data, DWORD size)
{
    ASSERT_ALWAYS(data && size >= 44);   // minimum WAV header

    DWORD fmtSize = 0, fmtPos = 0;
    if (FAILED(FindChunk(data, size, 0x20746d66, fmtSize, fmtPos))) // 'fmt '
        return E_FAIL;

    if (FAILED(ReadChunkData(data, fmtPos, &m_format, fmtSize)))
        return E_FAIL;

    DWORD dataSize = 0, dataPos = 0;
    if (FAILED(FindChunk(data, size, 0x61746164, dataSize, dataPos))) // 'data'
        return E_FAIL;

    m_audioData.resize(dataSize);
    if (FAILED(ReadChunkData(data, dataPos, m_audioData.data(), dataSize)))
        return E_FAIL;

    m_audioDataSize = dataSize;
    std::wcout << L"[INFO] WAV file parsed successfully. Audio data size: " << m_audioDataSize << std::endl;
    return S_OK;
}

HRESULT SoundEffect::FindChunk(const BYTE* data, DWORD dataSize,
    DWORD fourCC, DWORD& outSize, DWORD& outPos)
{
    ASSERT(dataSize > 12);          // RIFF header size
    DWORD offset = 12;              // skip RIFF + WAVE ids

    while (offset + 8 <= dataSize)
    {
        DWORD type = *reinterpret_cast<const DWORD*>(data + offset);
        DWORD size = *reinterpret_cast<const DWORD*>(data + offset + 4);

        if (type == fourCC)
        {
            outSize = size;
            outPos = offset + 8;
            return S_OK;
        }
        offset += 8 + size + (size & 1); // pad to word
    }
    return E_FAIL;
}

HRESULT SoundEffect::ReadChunkData(const BYTE* src, DWORD pos, void* dst, DWORD bytes)
{
    memcpy(dst, src + pos, bytes);
    return S_OK;
}

//------------------------------------------------------------------------------
//  SoundEffectFactory implementation
//------------------------------------------------------------------------------
static constexpr float PI = 3.14159265358979f;

void SoundEffectFactory::GenerateWaveform(std::vector<short>& samples,
    float freq, float dur,
    float (*wave)(float))
{
    const DWORD SR = 44100;
    const DWORD count = static_cast<DWORD>(dur * SR);
    ASSERT_ALWAYS(count);

    samples.resize(count);
    for (DWORD i = 0; i < count; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float phase = 2.f * PI * freq * t;
        float sample = wave(phase);
        samples[i] = static_cast<short>(std::clamp(sample, -1.f, 1.f) * 32767.f);
    }
}

std::unique_ptr<SoundEffect>
SoundEffectFactory::CreateFromSamples(const std::vector<short>& samples, DWORD SR)
{
    ASSERT_ALWAYS(!samples.empty());

    auto se = std::make_unique<SoundEffect>();
    const BYTE* bytes = reinterpret_cast<const BYTE*>(samples.data());
    DWORD size = static_cast<DWORD>(samples.size() * sizeof(short));
    if (FAILED(se->LoadFromMemory(bytes, size)))
        return nullptr;
    // patch the format to 44.1 kHz / mono / 16-bit
    se->m_format.wFormatTag = WAVE_FORMAT_PCM;
    se->m_format.nChannels = 1;
    se->m_format.nSamplesPerSec = SR;
    se->m_format.wBitsPerSample = 16;
    se->m_format.nBlockAlign = se->m_format.nChannels * se->m_format.wBitsPerSample / 8;
    se->m_format.nAvgBytesPerSec = se->m_format.nSamplesPerSec * se->m_format.nBlockAlign;
    std::wcout << L"[INFO] SoundEffectFactory::CreateFromSamples completed." << std::endl;
    return se;
}

// ----- basic wave helpers ----------------------------------------------------
float SoundEffectFactory::SineWave(float t) { return std::sin(t); }

float SoundEffectFactory::NoiseWave(float)
{
    static thread_local std::mt19937 gen{ std::random_device{}() };
    static thread_local std::uniform_real_distribution<float> dist(-1.f, 1.f);
    return dist(gen);
}

// ----- simple tones ----------------------------------------------------------
std::unique_ptr<SoundEffect>
SoundEffectFactory::CreateSine(float f, float d)
{
    std::vector<short> s;
    GenerateWaveform(s, f, d, SineWave);
    return CreateFromSamples(s);
}

std::unique_ptr<SoundEffect>
SoundEffectFactory::CreateBeep(float f, float d) { return CreateSine(f, d); }

std::unique_ptr<SoundEffect>
SoundEffectFactory::CreateNoise(float d)
{
    std::vector<short> s;
    GenerateWaveform(s, 0.f, d, NoiseWave);
    return CreateFromSamples(s);
}

// ----- game-specific SFX -----------------------------------------------------
std::unique_ptr<SoundEffect> SoundEffectFactory::CreateGunshot()
{
    const DWORD SR = 44100;
    const float DUR = 0.12f;
    const DWORD CNT = static_cast<DWORD>(SR * DUR);

    std::vector<short> s(CNT);
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> rnd(-1.f, 1.f);

    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float env = std::exp(-t * 45.f);            // fast decay
        s[i] = static_cast<short>(rnd(rng) * env * 32767.f);
    }
    return CreateFromSamples(s, SR);
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreateExplosion()
{
    const DWORD SR = 44100;
    const float DUR = 1.f;
    const DWORD CNT = static_cast<DWORD>(SR * DUR);

    std::vector<short> s(CNT);
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> rnd(-1.f, 1.f);

    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float env = std::exp(-t * 3.f);
        float rumble = std::sin(2.f * PI * 60.f * t) * 0.5f;
        float noise = rnd(rng) * 0.35f;
        s[i] = static_cast<short>((rumble + noise) * env * 32767.f);
    }
    return CreateFromSamples(s, SR);
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreateFootstep()
{
    const DWORD SR = 44100;
    const float DUR = 0.25f;
    const DWORD CNT = static_cast<DWORD>(SR * DUR);

    std::vector<short> s(CNT);
    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float env = std::exp(-t * 22.f);
        float thp = std::sin(2.f * PI * 110.f * t);
        s[i] = static_cast<short>(thp * env * 16383.f);  // half volume
    }
    return CreateFromSamples(s, SR);
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreateReload()
{
    const DWORD SR = 44100;
    const float DUR = 0.35f;
    const DWORD CNT = static_cast<DWORD>(SR * DUR);

    std::vector<short> s(CNT);
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> rnd(-0.3f, 0.3f);

    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float sample = 0.f;

        // metallic click at start
        if (t < 0.05f)
            sample = std::sin(2.f * PI * 2000.f * t) * std::exp(-t * 60.f);
        // metallic click at end
        else if (t > 0.28f)
        {
            float tt = t - 0.28f;
            sample = std::sin(2.f * PI * 1600.f * tt) * std::exp(-tt * 55.f);
        }
        // add subtle noise
        sample += rnd(rng) * 0.08f * std::exp(-t * 6.f);
        s[i] = static_cast<short>(sample * 16383.f);
    }
    return CreateFromSamples(s, SR);
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreatePickup()
{
    const DWORD SR = 44100;
    const float DUR = 0.28f;
    const DWORD CNT = static_cast<DWORD>(SR * DUR);

    std::vector<short> s(CNT);
    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float prog = t / DUR;
        float freq = 440.f + 440.f * prog;   // glide 440->880
        float env = 1.f - prog;             // fade out
        float samp = std::sin(2.f * PI * freq * t) * env;
        s[i] = static_cast<short>(samp * 16383.f);
    }
    return CreateFromSamples(s, SR);
}
