#include "SoundEffect.h"
#include "Core/Platform.h"
#include "Utils/Assert.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include "../Utils/Validate.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

//------------------------------------------------------------------------------
//  SoundEffect implementation
//------------------------------------------------------------------------------
SoundEffect::SoundEffect() : m_audioDataSize(0)
{
    ZeroMemory(&m_format, sizeof(m_format));
}

SoundEffect::~SoundEffect()
{
    Unload();
}

HRESULT SoundEffect::LoadFromFile(const std::wstring& filename)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
    if (filename.empty())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect::LoadFromFile - empty filename");
        return E_INVALIDARG;
    }

#if defined(SPARK_PLATFORM_WINDOWS) && defined(_MSC_VER)
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
#else
    // MinGW and Linux: convert wstring to narrow string for ifstream
    std::string narrowFilename(filename.begin(), filename.end());
    std::ifstream file(narrowFilename, std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open())
    {
#if defined(SPARK_PLATFORM_WINDOWS) && defined(_MSC_VER)
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: Failed to open WAV file '%ls' (errno=%d)",
                        filename.c_str(), errno);
#else
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: Failed to open WAV file '%s' (errno=%d)",
                        narrowFilename.c_str(), errno);
#endif
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    std::streamsize size = file.tellg();
    if (size <= 0)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: Zero-length or unreadable WAV file");
        return E_FAIL;
    }
    // Reject files larger than 256 MB up-front so a corrupted/huge file
    // cannot trigger an unbounded allocation.
    constexpr std::streamsize kMaxWavBytes = 256ll * 1024ll * 1024ll;
    if (size > kMaxWavBytes)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: WAV file too large (%lld bytes)",
                        static_cast<long long>(size));
        return E_FAIL;
    }
    file.seekg(0, std::ios::beg);

    std::vector<BYTE> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: WAV read incomplete (expected %lld bytes, got %lld)",
                        static_cast<long long>(size), static_cast<long long>(file.gcount()));
        return E_FAIL;
    }

    return ParseWAVFile(buffer.data(), static_cast<DWORD>(size));
}

HRESULT SoundEffect::LoadFromMemory(const BYTE* data, DWORD dataSize)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Audio);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Audio, data);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Audio, dataSize > 0, "LoadFromMemory called with zero dataSize");
    return ParseWAVFile(data, dataSize);
}

void SoundEffect::Unload()
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Audio, "SoundEffect: Unloading sound data (%lu bytes)",
                    static_cast<unsigned long>(m_audioDataSize));
    m_audioData.clear();
    m_audioDataSize = 0;
    ZeroMemory(&m_format, sizeof(m_format));
}

float SoundEffect::GetDuration() const
{
    return (m_format.nAvgBytesPerSec == 0) ? 0.f : static_cast<float>(m_audioDataSize) / m_format.nAvgBytesPerSec;
}

// ---------------------------------------------------------------------------
//  Private helpers
// ---------------------------------------------------------------------------
namespace
{
    // XAudio2's accepted source-voice sample-rate range (XAUDIO2_MIN/MAX_SAMPLE_RATE).
    constexpr DWORD kMinWavSampleRate = 1000;
    constexpr DWORD kMaxWavSampleRate = 200000;
    // 7.1 is the widest layout the engine's voices and mixer are built for.
    constexpr WORD kMaxWavChannels = 8;
    // WAVEFORMAT (tag, channels, rate, byte rate, block align) + wBitsPerSample.
    constexpr DWORD kMinFmtChunkBytes = 16;

    /**
     * @brief Check a file-supplied fmt chunk before it reaches XAudio2.
     *
     * The loader stores a bare 18-byte WAVEFORMATEX, and XAudio2 reads
     * 18 + cbSize bytes interpreted by wFormatTag. Only the plain PCM and
     * IEEE-float layouts fit that storage, so every other tag (EXTENSIBLE,
     * ADPCM, ...) is rejected and the caller forces cbSize to 0. The derived
     * fields must agree with each other so no consumer (XAudio2, GetDuration,
     * buffer submission) sees a block size that does not divide the data.
     */
    bool IsSupportedWavFormat(const WAVEFORMATEX& format, DWORD fmtChunkSize, DWORD dataSize)
    {
        if (fmtChunkSize < kMinFmtChunkBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: WAV 'fmt ' chunk too small (%lu bytes)",
                            static_cast<unsigned long>(fmtChunkSize));
            return false;
        }

        const bool isPcm = format.wFormatTag == WAVE_FORMAT_PCM;
        const bool isFloat = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (!isPcm && !isFloat)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Audio,
                            "SoundEffect: unsupported WAV format tag 0x%04X (only PCM and IEEE float are accepted)",
                            static_cast<unsigned>(format.wFormatTag));
            return false;
        }

        const WORD bits = format.wBitsPerSample;
        const bool bitsOk = isPcm ? (bits == 8 || bits == 16 || bits == 24 || bits == 32) : (bits == 32);
        if (!bitsOk || format.nChannels == 0 || format.nChannels > kMaxWavChannels)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: unsupported WAV layout (%u channels, %u bits)",
                            static_cast<unsigned>(format.nChannels), static_cast<unsigned>(bits));
            return false;
        }

        if (format.nSamplesPerSec < kMinWavSampleRate || format.nSamplesPerSec > kMaxWavSampleRate)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: WAV sample rate %lu Hz out of range",
                            static_cast<unsigned long>(format.nSamplesPerSec));
            return false;
        }

        // Bounded above: 8 channels * 32 bits / 8 = 32 bytes per block, and
        // 200000 Hz * 32 bytes fits a DWORD, so neither product can overflow.
        const DWORD expectedBlockAlign = static_cast<DWORD>(format.nChannels) * bits / 8u;
        const DWORD expectedByteRate = format.nSamplesPerSec * expectedBlockAlign;
        if (format.nBlockAlign != expectedBlockAlign || format.nAvgBytesPerSec != expectedByteRate)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Audio,
                            "SoundEffect: inconsistent WAV header (blockAlign=%u expected %lu, byteRate=%lu "
                            "expected %lu)",
                            static_cast<unsigned>(format.nBlockAlign), static_cast<unsigned long>(expectedBlockAlign),
                            static_cast<unsigned long>(format.nAvgBytesPerSec),
                            static_cast<unsigned long>(expectedByteRate));
            return false;
        }

        if (dataSize % expectedBlockAlign != 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Audio,
                            "SoundEffect: WAV 'data' size %lu is not a whole number of %lu-byte frames",
                            static_cast<unsigned long>(dataSize), static_cast<unsigned long>(expectedBlockAlign));
            return false;
        }
        return true;
    }
} // namespace

HRESULT SoundEffect::ParseWAVFile(const BYTE* data, DWORD size)
{
    // Parse into locals and commit only on success, so a rejected file leaves
    // this object unloaded instead of half-updated with an unvalidated format.
    Unload();

    if (!data || size < 44)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: WAV data too small (%lu bytes)",
                        static_cast<unsigned long>(size));
        return E_FAIL;
    }

    DWORD fmtSize = 0, fmtPos = 0;
    if (FAILED(FindChunk(data, size, 0x20746d66, fmtSize, fmtPos))) // 'fmt '
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: 'fmt ' chunk not found in WAV data");
        return E_FAIL;
    }

    // Copy at most the 18-byte WAVEFORMATEX; any extension bytes in the file
    // are ignored, because only formats without an extension are accepted.
    WAVEFORMATEX format;
    ZeroMemory(&format, sizeof(format));
    const DWORD fmtCopy = std::min(fmtSize, static_cast<DWORD>(sizeof(format)));
    if (FAILED(ReadChunkData(data, size, fmtPos, &format, fmtCopy)))
        return E_FAIL;

    DWORD dataSize = 0, dataPos = 0;
    if (FAILED(FindChunk(data, size, 0x61746164, dataSize, dataPos))) // 'data'
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: 'data' chunk not found in WAV data");
        return E_FAIL;
    }

    // Validate that the declared data chunk actually fits in the buffer.
    if (dataPos > size || dataSize > size - dataPos)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio,
                        "SoundEffect: WAV 'data' chunk out of range (pos=%lu size=%lu fileSize=%lu)",
                        static_cast<unsigned long>(dataPos), static_cast<unsigned long>(dataSize),
                        static_cast<unsigned long>(size));
        return E_FAIL;
    }

    if (!IsSupportedWavFormat(format, fmtSize, dataSize))
        return E_FAIL;
    // PCM and IEEE float carry no extension: XAudio2 must read exactly the
    // 18 bytes this object stores, whatever the file wrote into cbSize.
    format.cbSize = 0;

    std::vector<BYTE> audio(dataSize);
    if (FAILED(ReadChunkData(data, size, dataPos, audio.data(), dataSize)))
    {
        return E_FAIL;
    }

    m_format = format;
    m_audioData = std::move(audio);
    m_audioDataSize = dataSize;
    SPARK_LOG_INFO(Spark::LogCategory::Audio, "SoundEffect: WAV loaded successfully, data size: %lu bytes",
                   static_cast<unsigned long>(dataSize));
    return S_OK;
}

HRESULT SoundEffect::FindChunk(const BYTE* data, DWORD dataSize, DWORD fourCC, DWORD& outSize, DWORD& outPos)
{
    if (dataSize <= 12)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Audio, "SoundEffect: WAV data too small for RIFF header");
        return E_FAIL;
    }
    DWORD offset = 12; // skip RIFF + WAVE ids

    // offset can end one past dataSize (a final odd chunk without its pad
    // byte), so compare by subtraction instead of letting offset + 8 wrap.
    while (offset < dataSize && dataSize - offset >= 8)
    {
        // RIFF chunk ids and sizes are 32-bit little-endian fields at 2-byte
        // aligned offsets. Read them as std::uint32_t through memcpy: DWORD is
        // 8 bytes on LP64 (Core/PlatformTypes.h), and a typed load would also
        // be misaligned.
        std::uint32_t type = 0;
        std::uint32_t size = 0;
        std::memcpy(&type, data + offset, sizeof(type));
        std::memcpy(&size, data + offset + 4, sizeof(size));

        // Validate that the chunk body fits entirely in the buffer. Without
        // this check a corrupted size field would let us read past the end
        // when a caller subsequently copies data out.
        const DWORD bodyStart = offset + 8;
        if (size > dataSize || bodyStart > dataSize || size > dataSize - bodyStart)
            return E_FAIL;

        if (type == fourCC)
        {
            outSize = size;
            outPos = bodyStart;
            return S_OK;
        }
        // Advance with explicit overflow check on the pad byte.
        DWORD pad = size & 1u;
        if (size > 0xFFFFFFFFu - 8u - pad)
            return E_FAIL;
        DWORD step = 8u + size + pad;
        if (offset > 0xFFFFFFFFu - step)
            return E_FAIL;
        offset += step;
    }
    return E_FAIL;
}

HRESULT SoundEffect::ReadChunkData(const BYTE* src, DWORD srcSize, DWORD pos, void* dst, DWORD bytes)
{
    if (!src || !dst)
        return E_POINTER;
    if (pos > srcSize || bytes > srcSize - pos)
        return E_FAIL;
    memcpy(dst, src + pos, bytes);
    return S_OK;
}

//------------------------------------------------------------------------------
//  SoundEffectFactory implementation
//------------------------------------------------------------------------------

void SoundEffectFactory::GenerateWaveform(std::vector<short>& samples, float freq, float dur, float (*wave)(float))
{
    const DWORD SR = 44100;
    const DWORD count = static_cast<DWORD>(dur * SR);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Audio, count > 0, "GenerateWaveform: sample count must be positive");

    samples.resize(count);
    for (DWORD i = 0; i < count; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float phase = MathUtils::TWO_PI * freq * t;
        float sample = wave(phase);
        samples[i] = static_cast<short>(std::clamp(sample, -1.f, 1.f) * 32767.f);
    }
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreateFromSamples(const std::vector<short>& samples, DWORD SR)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Audio, !samples.empty(), "CreateFromSamples: samples must not be empty");

    auto se = std::make_unique<SoundEffect>();
    SPARK_LOG_DEBUG(Spark::LogCategory::Audio, "SoundEffect: Creating from %zu samples at %lu Hz", samples.size(),
                    static_cast<unsigned long>(SR));

    // Set up the PCM format directly (mono, 16-bit)
    se->m_format.wFormatTag = WAVE_FORMAT_PCM;
    se->m_format.nChannels = 1;
    se->m_format.nSamplesPerSec = SR;
    se->m_format.wBitsPerSample = 16;
    se->m_format.nBlockAlign = se->m_format.nChannels * se->m_format.wBitsPerSample / 8;
    se->m_format.nAvgBytesPerSec = se->m_format.nSamplesPerSec * se->m_format.nBlockAlign;
    se->m_format.cbSize = 0;

    // Copy raw PCM sample data directly into the audio buffer
    DWORD dataSize = static_cast<DWORD>(samples.size() * sizeof(short));
    se->m_audioData.resize(dataSize);
    std::memcpy(se->m_audioData.data(), samples.data(), dataSize);
    se->m_audioDataSize = dataSize;

    return se;
}

// ----- basic wave helpers ----------------------------------------------------
float SoundEffectFactory::SineWave(float t)
{
    return std::sin(t);
}

float SoundEffectFactory::NoiseWave(float)
{
    static thread_local std::mt19937 gen{std::random_device{}()};
    static thread_local std::uniform_real_distribution<float> dist(-1.f, 1.f);
    return dist(gen);
}

// ----- simple tones ----------------------------------------------------------
std::unique_ptr<SoundEffect> SoundEffectFactory::CreateSine(float f, float d)
{
    std::vector<short> s;
    GenerateWaveform(s, f, d, SineWave);
    return CreateFromSamples(s);
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreateBeep(float f, float d)
{
    return CreateSine(f, d);
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreateNoise(float d)
{
    std::vector<short> s;
    GenerateWaveform(s, 0.f, d, NoiseWave);
    return CreateFromSamples(s);
}

// ----- game-specific SFX -----------------------------------------------------
std::unique_ptr<SoundEffect> SoundEffectFactory::CreateGunshot()
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Audio, "SoundEffectFactory: Creating gunshot SFX");
    const DWORD SR = 44100;
    const float DUR = 0.12f;
    const DWORD CNT = static_cast<DWORD>(SR * DUR);

    std::vector<short> s(CNT);
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> rnd(-1.f, 1.f);

    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float env = std::exp(-t * 45.f); // fast decay
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
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> rnd(-1.f, 1.f);

    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float env = std::exp(-t * 3.f);
        float rumble = std::sin(MathUtils::TWO_PI * 60.f * t) * 0.5f;
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
        float thp = std::sin(MathUtils::TWO_PI * 110.f * t);
        s[i] = static_cast<short>(thp * env * 16383.f); // half volume
    }
    return CreateFromSamples(s, SR);
}

std::unique_ptr<SoundEffect> SoundEffectFactory::CreateReload()
{
    const DWORD SR = 44100;
    const float DUR = 0.35f;
    const DWORD CNT = static_cast<DWORD>(SR * DUR);

    std::vector<short> s(CNT);
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> rnd(-0.3f, 0.3f);

    for (DWORD i = 0; i < CNT; ++i)
    {
        float t = static_cast<float>(i) / SR;
        float sample = 0.f;

        // metallic click at start
        if (t < 0.05f)
            sample = std::sin(MathUtils::TWO_PI * 2000.f * t) * std::exp(-t * 60.f);
        // metallic click at end
        else if (t > 0.28f)
        {
            float tt = t - 0.28f;
            sample = std::sin(MathUtils::TWO_PI * 1600.f * tt) * std::exp(-tt * 55.f);
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
        float freq = 440.f + 440.f * prog; // glide 440->880
        float env = 1.f - prog;            // fade out
        float samp = std::sin(MathUtils::TWO_PI * freq * t) * env;
        s[i] = static_cast<short>(samp * 16383.f);
    }
    return CreateFromSamples(s, SR);
}
