/**
 * @file FuzzSoundWavProduction.cpp
 * @brief libc++-compiled production adapter for the SoundEffect WAV libFuzzer harness.
 *
 * SoundEffect::LoadFromMemory is the in-memory entry of the WAV parser that
 * XAudio2 playback (SoundEffect/AudioEngine) and the OpenAL backend
 * (OpenALAudioEngine::LoadWAVFile) both decode through. The same SoundEffect is
 * reused for every input, so a rejected file also has to undo whatever the
 * previous accepted one left loaded. The result is checked against the
 * guarantees its consumers rely on; a violation aborts so libFuzzer records it
 * as a crash rather than a silent pass:
 *  - a successful load is loaded, and its sample bytes fit in the input after
 *    the smallest possible header (RIFF 12 + fmt 8+16 + data 8 = 44 bytes),
 *  - the stored format is PCM or IEEE float with nBlockAlign equal to
 *    channels * bits / 8, nAvgBytesPerSec equal to rate * nBlockAlign, a
 *    whole number of frames, and cbSize 0 (XAudio2 reads 18 + cbSize bytes),
 *  - every sample byte GetData() exposes is readable (ASan checks the copy),
 *  - a failed load leaves the object unloaded with no data.
 */

#include "FuzzSoundWavProduction.h"

#include "Audio/SoundEffect.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::size_t kMinimalWavHeaderBytes = 44;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzSoundWav: SoundEffect published a result that violates: %s\n", what);
        std::abort();
    }

    void CheckLoaded(const SoundEffect& sound, std::size_t inputSize)
    {
        if (!sound.IsLoaded())
            InvariantFailure("a successful load is not loaded");
        const std::size_t dataSize = sound.GetDataSize();
        if (inputSize < kMinimalWavHeaderBytes || dataSize > inputSize - kMinimalWavHeaderBytes)
            InvariantFailure("sample bytes exceed what the input holds after its header");

        const WAVEFORMATEX& format = sound.GetFormat();
        if (format.wFormatTag != WAVE_FORMAT_PCM && format.wFormatTag != WAVE_FORMAT_IEEE_FLOAT)
            InvariantFailure("format tag is neither PCM nor IEEE float");
        const std::size_t blockAlign = static_cast<std::size_t>(format.nChannels) * format.wBitsPerSample / 8u;
        if (blockAlign == 0 || format.nBlockAlign != blockAlign)
            InvariantFailure("nBlockAlign differs from channels * bits / 8");
        if (format.nAvgBytesPerSec != static_cast<std::size_t>(format.nSamplesPerSec) * blockAlign)
            InvariantFailure("nAvgBytesPerSec differs from rate * nBlockAlign");
        if (dataSize % blockAlign != 0)
            InvariantFailure("sample bytes are not a whole number of frames");
        if (format.cbSize != 0)
            InvariantFailure("cbSize is not 0");

        // Touch every exposed byte so ASan proves the whole region is owned.
        const BYTE* samples = sound.GetData();
        volatile std::uint32_t checksum = 0;
        for (std::size_t i = 0; i < dataSize; ++i)
            checksum = checksum * 31u + samples[i];
        (void)checksum;
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production parser and logger.
extern "C" int SparkFuzzLoadWav(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    // LoadFromMemory requires a non-null buffer of at least one byte and fails
    // its precondition otherwise (SPARK_REQUIRE); that is a caller contract,
    // not a parser outcome.
    if (data == nullptr || size == 0)
        return 0;

    static SoundEffect sound;
    if (SUCCEEDED(sound.LoadFromMemory(data, static_cast<DWORD>(size))))
    {
        CheckLoaded(sound, size);
    }
    else if (sound.IsLoaded() || sound.GetDataSize() != 0)
    {
        InvariantFailure("a failed load leaves the object loaded");
    }
    return 0;
}
