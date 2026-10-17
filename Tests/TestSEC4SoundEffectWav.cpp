/**
 * @file TestSEC4SoundEffectWav.cpp
 * @brief SEC4 regressions for the SoundEffect WAV fmt-chunk trust boundary
 *
 * SoundEffect stores a bare 18-byte WAVEFORMATEX and AudioEngine hands a
 * reference to it straight to IXAudio2::CreateSourceVoice, which reads
 * 18 + cbSize bytes interpreted by wFormatTag. A file that declared
 * WAVE_FORMAT_EXTENSIBLE or ADPCM with a nonzero cbSize therefore made XAudio2
 * read past the member into the object's vector pointers. The loader now
 * accepts only PCM / IEEE float with self-consistent fields, forces cbSize to
 * 0, and leaves the object unloaded when a file is rejected.
 *
 * These tests drive SoundEffect::LoadFromMemory directly; no audio device is
 * needed, so they run on every platform. The positive cases are what keep the
 * chunk walker honest off Windows: DWORD is 8 bytes on LP64 Linux/macOS, and a
 * DWORD-typed read of a RIFF id or size would never match 'fmt ' there.
 */

#include "TestFramework.h"

#include "Audio/SoundEffect.h"

#if !defined(SPARK_PLATFORM_WINDOWS)
#include "Audio/OpenALAudioEngine.h"
#endif

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace
{
    struct WavSpec
    {
        uint16_t formatTag = 1; // WAVE_FORMAT_PCM
        uint16_t channels = 1;
        uint32_t sampleRate = 44100;
        uint16_t bitsPerSample = 16;
        uint16_t blockAlign = 2;
        uint32_t byteRate = 88200;
        uint32_t fmtChunkSize = 16;    ///< Declared 'fmt ' size; bytes past 16 hold cbSize + extension
        uint16_t cbSize = 0;           ///< Present in the file only when fmtChunkSize >= 18
        uint32_t dataSize = 4410 * 2;  ///< 100 ms of mono 16-bit
        uint32_t leadingChunkSize = 0; ///< Nonzero: emit a 'LIST' chunk of this size before 'fmt '
    };

    void PushU16(std::vector<BYTE>& out, uint16_t value)
    {
        out.push_back(static_cast<BYTE>(value & 0xFFu));
        out.push_back(static_cast<BYTE>((value >> 8) & 0xFFu));
    }

    void PushU32(std::vector<BYTE>& out, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            out.push_back(static_cast<BYTE>((value >> shift) & 0xFFu));
        }
    }

    void PushTag(std::vector<BYTE>& out, const char* tag)
    {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<BYTE>(tag[i]));
        }
    }

    std::vector<BYTE> MakeWav(const WavSpec& spec)
    {
        // fmt body: the 16-byte WAVEFORMAT, then cbSize, then extension bytes
        // (SubFormat GUID, ADPCM coefficients, ...) as a recognisable pattern,
        // truncated or padded to the declared chunk size.
        std::vector<BYTE> fmt;
        PushU16(fmt, spec.formatTag);
        PushU16(fmt, spec.channels);
        PushU32(fmt, spec.sampleRate);
        PushU32(fmt, spec.byteRate);
        PushU16(fmt, spec.blockAlign);
        PushU16(fmt, spec.bitsPerSample);
        PushU16(fmt, spec.cbSize);
        fmt.resize(spec.fmtChunkSize, static_cast<BYTE>(0xA5));

        const uint32_t fmtPad = spec.fmtChunkSize & 1u;
        const uint32_t dataPad = spec.dataSize & 1u;
        const uint32_t leadingPad = spec.leadingChunkSize & 1u;
        const uint32_t leadingBytes = spec.leadingChunkSize == 0 ? 0u : 8u + spec.leadingChunkSize + leadingPad;

        std::vector<BYTE> wav;
        PushTag(wav, "RIFF");
        PushU32(wav, 4u + leadingBytes + (8u + spec.fmtChunkSize + fmtPad) + (8u + spec.dataSize + dataPad));
        PushTag(wav, "WAVE");
        if (spec.leadingChunkSize != 0)
        {
            PushTag(wav, "LIST");
            PushU32(wav, spec.leadingChunkSize);
            wav.insert(wav.end(), spec.leadingChunkSize + leadingPad, static_cast<BYTE>(0x5A));
        }
        PushTag(wav, "fmt ");
        PushU32(wav, spec.fmtChunkSize);
        wav.insert(wav.end(), fmt.begin(), fmt.end());
        wav.insert(wav.end(), fmtPad, static_cast<BYTE>(0));
        PushTag(wav, "data");
        PushU32(wav, spec.dataSize);
        wav.insert(wav.end(), spec.dataSize + dataPad, static_cast<BYTE>(0));
        return wav;
    }

    HRESULT Load(SoundEffect& sound, const std::vector<BYTE>& wav)
    {
        return sound.LoadFromMemory(wav.data(), static_cast<DWORD>(wav.size()));
    }
} // namespace

// A well-formed PCM file must keep loading, and a stray cbSize in an 18-byte
// PCM fmt chunk must not survive into the format XAudio2 reads.
TEST(SEC4SoundWav_AcceptsPcmAndFloatAndForcesZeroCbSize)
{
    SoundEffect pcm;
    WavSpec pcmSpec;
    pcmSpec.fmtChunkSize = 18;
    pcmSpec.cbSize = 22;
    ASSERT_TRUE(SUCCEEDED(Load(pcm, MakeWav(pcmSpec))));
    ASSERT_TRUE(pcm.IsLoaded());
    ASSERT_EQ(static_cast<int>(pcm.GetFormat().wFormatTag), 1);
    ASSERT_EQ(static_cast<int>(pcm.GetFormat().cbSize), 0);
    ASSERT_EQ(static_cast<unsigned long>(pcm.GetDataSize()), static_cast<unsigned long>(pcmSpec.dataSize));

    SoundEffect stereoFloat;
    WavSpec floatSpec;
    floatSpec.formatTag = 3; // WAVE_FORMAT_IEEE_FLOAT
    floatSpec.channels = 2;
    floatSpec.sampleRate = 48000;
    floatSpec.bitsPerSample = 32;
    floatSpec.blockAlign = 8;
    floatSpec.byteRate = 48000u * 8u;
    floatSpec.dataSize = 480u * 8u;
    ASSERT_TRUE(SUCCEEDED(Load(stereoFloat, MakeWav(floatSpec))));
    ASSERT_EQ(static_cast<int>(stereoFloat.GetFormat().wFormatTag), 3);
    ASSERT_EQ(static_cast<int>(stereoFloat.GetFormat().cbSize), 0);
}

// WAVE_FORMAT_EXTENSIBLE with cbSize=22 would make XAudio2 read 22 bytes past
// the 18-byte member the loader stores.
TEST(SEC4SoundWav_RejectsExtensibleFormatWithExtension)
{
    SoundEffect sound;
    WavSpec spec;
    spec.formatTag = 0xFFFE; // WAVE_FORMAT_EXTENSIBLE
    spec.fmtChunkSize = 40;
    spec.cbSize = 22;
    ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    ASSERT_FALSE(sound.IsLoaded());
    ASSERT_EQ(static_cast<int>(sound.GetFormat().wFormatTag), 0);
    ASSERT_EQ(static_cast<int>(sound.GetFormat().cbSize), 0);
}

// ADPCM (tag 2, cbSize 32) would make XAudio2 read coefficient tables that the
// loader never stored.
TEST(SEC4SoundWav_RejectsAdpcmFormat)
{
    SoundEffect sound;
    WavSpec spec;
    spec.formatTag = 2; // WAVE_FORMAT_ADPCM
    spec.bitsPerSample = 4;
    spec.blockAlign = 256;
    spec.byteRate = 22311;
    spec.fmtChunkSize = 50;
    spec.cbSize = 32;
    spec.dataSize = 256u * 4u;
    ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    ASSERT_FALSE(sound.IsLoaded());
}

// PCM headers whose derived fields disagree, or whose sizes cannot describe
// whole frames, are rejected instead of being left to the device.
TEST(SEC4SoundWav_RejectsInconsistentPcmHeaders)
{
    {
        SoundEffect sound;
        WavSpec spec;
        spec.blockAlign = 64; // mono 16-bit is 2 bytes per frame
        ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
        ASSERT_FALSE(sound.IsLoaded());
    }
    {
        SoundEffect sound;
        WavSpec spec;
        spec.byteRate = 1; // GetDuration would report hours of audio
        ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    }
    {
        SoundEffect sound;
        WavSpec spec;
        spec.channels = 0;
        spec.blockAlign = 0;
        spec.byteRate = 0;
        ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    }
    {
        SoundEffect sound;
        WavSpec spec;
        spec.bitsPerSample = 12;
        ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    }
    {
        SoundEffect sound;
        WavSpec spec;
        spec.sampleRate = 1;
        spec.byteRate = 2;
        ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    }
    {
        SoundEffect sound;
        WavSpec spec;
        spec.fmtChunkSize = 14; // wBitsPerSample missing
        ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    }
    {
        SoundEffect sound;
        WavSpec spec;
        spec.dataSize = 4411; // half a 16-bit frame at the end
        ASSERT_TRUE(FAILED(Load(sound, MakeWav(spec))));
    }
}

// Chunk ids and sizes are 32-bit fields at 2-byte aligned offsets. A 1-byte
// chunk plus its pad byte puts 'fmt ' at offset 22 and 'data' at offset 46,
// neither 4-aligned; the walker must still find both (and must not rely on a
// DWORD-typed load, which is 8 bytes wide on LP64 and misaligned here).
TEST(SEC4SoundWav_FindsChunksAtTwoByteAlignedOffsets)
{
    SoundEffect sound;
    WavSpec spec;
    spec.leadingChunkSize = 1;
    const std::vector<BYTE> wav = MakeWav(spec);
    ASSERT_EQ(static_cast<int>(wav[22]), static_cast<int>('f'));
    ASSERT_TRUE(SUCCEEDED(Load(sound, wav)));
    ASSERT_TRUE(sound.IsLoaded());
    ASSERT_EQ(static_cast<unsigned long>(sound.GetFormat().nSamplesPerSec), 44100ul);
    ASSERT_EQ(static_cast<unsigned long>(sound.GetDataSize()), static_cast<unsigned long>(spec.dataSize));

    // A final odd-sized chunk whose pad byte is missing ends the walk one byte
    // past the buffer; a search for an absent chunk must fail cleanly.
    SoundEffect truncated;
    std::vector<BYTE> noData = MakeWav(WavSpec{});
    noData.resize(12u + 8u + 16u); // RIFF header + 'fmt ' only
    noData.push_back('J');
    noData.push_back('U');
    noData.push_back('N');
    noData.push_back('K');
    for (BYTE b : {BYTE{1}, BYTE{0}, BYTE{0}, BYTE{0}, BYTE{0x77}})
    {
        noData.push_back(b);
    }
    ASSERT_TRUE(FAILED(Load(truncated, noData)));
    ASSERT_FALSE(truncated.IsLoaded());
}

// A rejected reload must not leave the previous (or a half-parsed) format behind.
TEST(SEC4SoundWav_RejectedReloadLeavesSoundUnloaded)
{
    SoundEffect sound;
    ASSERT_TRUE(SUCCEEDED(Load(sound, MakeWav(WavSpec{}))));
    ASSERT_TRUE(sound.IsLoaded());

    WavSpec bad;
    bad.formatTag = 0xFFFE;
    bad.fmtChunkSize = 40;
    bad.cbSize = 22;
    ASSERT_TRUE(FAILED(Load(sound, MakeWav(bad))));
    ASSERT_FALSE(sound.IsLoaded());
    ASSERT_EQ(static_cast<unsigned long>(sound.GetDataSize()), 0ul);
    ASSERT_EQ(static_cast<int>(sound.GetFormat().wFormatTag), 0);
    ASSERT_EQ(static_cast<unsigned long>(sound.GetFormat().nAvgBytesPerSec), 0ul);
}

#if !defined(SPARK_PLATFORM_WINDOWS)
// OpenALAudioEngine::LoadWAVFile decodes through SoundEffect::LoadFromMemory and
// hands alBufferData GetData()/GetDataSize(), GetFormat().nSamplesPerSec and the
// format SelectOpenALWavFormat picks. The OpenAL device half needs audio
// hardware, so these pin the decoding half on every non-Windows build.
namespace
{
    // AL/al.h values, which OpenALAudioEngine.cpp also uses when OpenAL is absent.
    constexpr int kAlFormatMono8 = 0x1100;
    constexpr int kAlFormatMono16 = 0x1101;
    constexpr int kAlFormatStereo8 = 0x1102;
    constexpr int kAlFormatStereo16 = 0x1103;
} // namespace

TEST(SEC4OpenALWav_PcmLayoutsMapToCoreFormats)
{
    SoundEffect mono16;
    const WavSpec monoSpec;
    ASSERT_TRUE(SUCCEEDED(Load(mono16, MakeWav(monoSpec))));
    ASSERT_EQ(Spark::Audio::SelectOpenALWavFormat(mono16.GetFormat()), kAlFormatMono16);
    ASSERT_EQ(static_cast<unsigned long>(mono16.GetFormat().nSamplesPerSec), 44100ul);
    ASSERT_EQ(static_cast<unsigned long>(mono16.GetDataSize()), static_cast<unsigned long>(monoSpec.dataSize));
    ASSERT_TRUE(mono16.GetData() != nullptr);

    struct Layout
    {
        uint16_t channels;
        uint16_t bits;
        int alFormat;
    };
    for (const Layout& layout :
         {Layout{1, 8, kAlFormatMono8}, Layout{2, 8, kAlFormatStereo8}, Layout{2, 16, kAlFormatStereo16}})
    {
        SoundEffect sound;
        WavSpec spec;
        spec.channels = layout.channels;
        spec.bitsPerSample = layout.bits;
        spec.blockAlign = static_cast<uint16_t>(layout.channels * layout.bits / 8);
        spec.byteRate = spec.sampleRate * spec.blockAlign;
        spec.dataSize = 64u * spec.blockAlign;
        ASSERT_TRUE(SUCCEEDED(Load(sound, MakeWav(spec))));
        ASSERT_EQ(Spark::Audio::SelectOpenALWavFormat(sound.GetFormat()), layout.alFormat);
    }
}

// SoundEffect accepts IEEE float, 24/32-bit PCM and up to 8 channels, none of
// which OpenAL's core formats hold; LoadWAVFile must refuse them instead of
// guessing a format from the channel and bit counts.
TEST(SEC4OpenALWav_RefusesLayoutsOutsideCoreFormats)
{
    SoundEffect stereoFloat;
    WavSpec floatSpec;
    floatSpec.formatTag = 3; // WAVE_FORMAT_IEEE_FLOAT
    floatSpec.channels = 2;
    floatSpec.bitsPerSample = 32;
    floatSpec.blockAlign = 8;
    floatSpec.byteRate = 44100u * 8u;
    floatSpec.dataSize = 64u * 8u;
    ASSERT_TRUE(SUCCEEDED(Load(stereoFloat, MakeWav(floatSpec))));
    ASSERT_EQ(Spark::Audio::SelectOpenALWavFormat(stereoFloat.GetFormat()), 0);

    SoundEffect pcm24;
    WavSpec pcm24Spec;
    pcm24Spec.bitsPerSample = 24;
    pcm24Spec.blockAlign = 3;
    pcm24Spec.byteRate = 44100u * 3u;
    pcm24Spec.dataSize = 64u * 3u;
    ASSERT_TRUE(SUCCEEDED(Load(pcm24, MakeWav(pcm24Spec))));
    ASSERT_EQ(Spark::Audio::SelectOpenALWavFormat(pcm24.GetFormat()), 0);

    SoundEffect surround;
    WavSpec surroundSpec;
    surroundSpec.channels = 6;
    surroundSpec.blockAlign = 12;
    surroundSpec.byteRate = 44100u * 12u;
    surroundSpec.dataSize = 64u * 12u;
    ASSERT_TRUE(SUCCEEDED(Load(surround, MakeWav(surroundSpec))));
    ASSERT_EQ(Spark::Audio::SelectOpenALWavFormat(surround.GetFormat()), 0);
}
#endif
