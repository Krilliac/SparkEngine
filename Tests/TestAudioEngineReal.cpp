/**
 * @file TestAudioEngineReal.cpp
 * @brief Real-class tests for AudioEngine / Spark::Audio::AudioMixer
 *
 * Covers the audio release-readiness fixes:
 *  - pooled XAudio2 voices must be recreated when the sound format changes
 *  - the volume chain is request * category (master lives on the mastering
 *    voice and must not be squared into every source)
 *  - mix bus volume, mute and solo actually scale playback
 *  - the 3D listener can be driven from a camera, with a real velocity
 *  - a pooled source handle is invalidated when the slot is recycled
 *  - device loss is detectable instead of degrading to permanent silence
 *  - occlusion reports "not measured" instead of faking "unoccluded"
 *
 * The device-dependent cases open a real output device; when none exists they
 * assert the no-device contract and then SKIP_TEST, so the runner reports them
 * as skipped rather than passed - a headless CI run that never exercised the
 * voice-rebuild, recycling or device-loss paths is distinguishable from one that
 * did. Every rule they check is also covered by a device-free case in this file,
 * so a machine without audio still fails these tests if the logic regresses.
 */

#include "TestFramework.h"

#include "Audio/AudioEngine.h"
#include "Audio/AudioMixer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    WAVEFORMATEX MakeFormat(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample)
    {
        WAVEFORMATEX format{};
        format.wFormatTag = 1; // WAVE_FORMAT_PCM
        format.nChannels = channels;
        format.nSamplesPerSec = sampleRate;
        format.wBitsPerSample = bitsPerSample;
        format.nBlockAlign = static_cast<uint16_t>(channels * bitsPerSample / 8);
        format.nAvgBytesPerSec = sampleRate * format.nBlockAlign;
        format.cbSize = 0;
        return format;
    }

    /** @brief Build a minimal 16-bit PCM WAV holding 50 ms of silence. */
    std::vector<unsigned char> MakeSilentWav(uint32_t sampleRate, uint16_t channels)
    {
        const uint16_t bitsPerSample = 16;
        const uint16_t blockAlign = static_cast<uint16_t>(channels * bitsPerSample / 8);
        const uint32_t byteRate = sampleRate * blockAlign;
        const uint32_t dataSize = (sampleRate / 20) * blockAlign;

        std::vector<unsigned char> wav;
        const auto pushU32 = [&wav](uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                wav.push_back(static_cast<unsigned char>((value >> shift) & 0xFFu));
            }
        };
        const auto pushU16 = [&wav](uint16_t value)
        {
            wav.push_back(static_cast<unsigned char>(value & 0xFFu));
            wav.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
        };
        const auto pushTag = [&wav](const char* tag)
        {
            for (int i = 0; i < 4; ++i)
            {
                wav.push_back(static_cast<unsigned char>(tag[i]));
            }
        };

        pushTag("RIFF");
        pushU32(36u + dataSize);
        pushTag("WAVE");
        pushTag("fmt ");
        pushU32(16u);
        pushU16(1u); // PCM
        pushU16(channels);
        pushU32(sampleRate);
        pushU32(byteRate);
        pushU16(blockAlign);
        pushU16(bitsPerSample);
        pushTag("data");
        pushU32(dataSize);
        wav.insert(wav.end(), dataSize, static_cast<unsigned char>(0));
        return wav;
    }

    /** @brief Write a silent WAV next to the test binary; returns "" on failure. */
    std::wstring WriteSilentWav(const std::wstring& stem, uint32_t sampleRate, uint16_t channels)
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "SparkAudioEngineReal";
        if (ec)
        {
            return std::wstring();
        }
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path file = dir / (stem + L".wav");

        const std::vector<unsigned char> wav = MakeSilentWav(sampleRate, channels);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return std::wstring();
        }
        out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        out.close();
        return file.wstring();
    }
} // namespace

// ============================================================================
// Format reuse (physics-audio-input-camera-01)
// ============================================================================

TEST(AudioEngineReal_FormatsCompatibleRejectsDifferentWaveFormats)
{
    const WAVEFORMATEX lowRateMono = MakeFormat(22050, 1, 16);
    const WAVEFORMATEX highRateStereo = MakeFormat(44100, 2, 16);
    const WAVEFORMATEX lowRateMonoAgain = MakeFormat(22050, 1, 16);

    EXPECT_TRUE(AudioEngine::FormatsCompatible(lowRateMono, lowRateMonoAgain));
    EXPECT_FALSE(AudioEngine::FormatsCompatible(lowRateMono, highRateStereo));
    EXPECT_FALSE(AudioEngine::FormatsCompatible(lowRateMono, MakeFormat(44100, 1, 16)));
    EXPECT_FALSE(AudioEngine::FormatsCompatible(lowRateMono, MakeFormat(22050, 2, 16)));
    EXPECT_FALSE(AudioEngine::FormatsCompatible(lowRateMono, MakeFormat(22050, 1, 8)));
}

TEST(AudioEngineReal_PooledVoiceIsRebuiltForANewSoundFormat)
{
    // Linux/macOS builds carry no-op audio stubs: Initialize() succeeds but no
    // sound can be loaded or played, so the XAudio2 semantics under test do not
    // exist there. Skip explicitly instead of failing on the first LoadSound.
    if (!AudioEngine::IsAudioBackendAvailable())
        SKIP_TEST("no audio backend on this platform (XAudio2 is Windows-only; stubs are no-ops)");

    AudioEngine engine;
    if (FAILED(engine.Initialize(1)))
    {
        // No output device here. Returning would report a PASS for a run that never
        // touched the voice-rebuild path this test exists to cover, so report a real
        // skip instead: the runner counts it apart from passes.
        EXPECT_FALSE(engine.IsAvailable());
        SKIP_TEST("no XAudio2 output device: the pooled-voice format rebuild cannot be exercised");
    }

    const std::wstring highRate = WriteSilentWav(L"spark_audio_44100_stereo", 44100, 2);
    const std::wstring lowRate = WriteSilentWav(L"spark_audio_22050_mono", 22050, 1);
    ASSERT_FALSE(highRate.empty());
    ASSERT_FALSE(lowRate.empty());
    ASSERT_TRUE(SUCCEEDED(engine.LoadSound("highRate", highRate)));
    ASSERT_TRUE(SUCCEEDED(engine.LoadSound("lowRate", lowRate)));

    AudioSource* first = engine.PlaySound("highRate");
    ASSERT_TRUE(first != nullptr);
    EXPECT_TRUE(first->HasVoiceFormat);
    EXPECT_EQ(static_cast<uint32_t>(first->VoiceFormat.nSamplesPerSec), 44100u);
    EXPECT_EQ(static_cast<uint32_t>(first->VoiceFormat.nChannels), 2u);
    engine.StopSound(first);

    // The single pooled slot comes back with a voice built for 44.1 kHz stereo.
    AudioSource* second = engine.PlaySound("lowRate");
    ASSERT_TRUE(second != nullptr);
    EXPECT_TRUE(second == first); // same pooled slot
    EXPECT_TRUE(second->HasVoiceFormat);
    EXPECT_EQ(static_cast<uint32_t>(second->VoiceFormat.nSamplesPerSec), 22050u);
    EXPECT_EQ(static_cast<uint32_t>(second->VoiceFormat.nChannels), 1u);

    engine.StopAllSounds();
    engine.Shutdown();
}

// ============================================================================
// Volume model (physics-audio-input-camera-17)
// ============================================================================

TEST(AudioEngineReal_CategoryVolumeExcludesMasterAndTracksBuses)
{
    AudioEngine engine;
    engine.SetMasterVolume(0.5f);
    engine.SetSFXVolume(0.5f);
    engine.SetMusicVolume(0.25f);

    // Master is applied once, on the mastering voice. Folding it in here as
    // well is what made master 0.5 behave like 0.25.
    EXPECT_NEAR(engine.GetCategoryVolume(AudioCategory::SFX), 0.5f, 0.0001f);
    EXPECT_NEAR(engine.GetCategoryVolume(AudioCategory::Music), 0.25f, 0.0001f);

    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    mixer.SetBusVolume("SFX", 0.5f);
    engine.SetMixer(&mixer);

    EXPECT_NEAR(engine.GetCategoryVolume(AudioCategory::SFX), 0.25f, 0.0001f);
    EXPECT_NEAR(engine.GetCategoryVolume(AudioCategory::Music), 0.25f, 0.0001f);

    mixer.SetBusMuted("Master", true);
    EXPECT_NEAR(engine.GetCategoryVolume(AudioCategory::SFX), 0.0f, 0.0001f);
    mixer.SetBusMuted("Master", false);

    engine.SetMixer(nullptr);
    EXPECT_NEAR(engine.GetCategoryVolume(AudioCategory::SFX), 0.5f, 0.0001f);
}

TEST(AudioEngineReal_LiveSourcesFollowCategoryVolumeChanges)
{
    if (!AudioEngine::IsAudioBackendAvailable())
        SKIP_TEST("no audio backend on this platform (XAudio2 is Windows-only; stubs are no-ops)");

    AudioEngine engine;
    if (FAILED(engine.Initialize(2)))
    {
        EXPECT_FALSE(engine.IsAvailable());
        SKIP_TEST("no XAudio2 output device: live-source volume propagation cannot be exercised");
    }

    const std::wstring path = WriteSilentWav(L"spark_audio_volume_case", 44100, 1);
    ASSERT_FALSE(path.empty());
    ASSERT_TRUE(SUCCEEDED(engine.LoadSound("cue", path)));

    engine.SetMasterVolume(0.5f);
    engine.SetSFXVolume(1.0f);

    AudioSource* source = engine.PlaySound("cue", 0.8f);
    ASSERT_TRUE(source != nullptr);
    EXPECT_NEAR(source->RequestedVolume, 0.8f, 0.0001f);
    // 0.8 * sfx(1.0); master must NOT appear here a second time.
    EXPECT_NEAR(source->Volume, 0.8f, 0.0001f);

    // Storing the field alone used to leave already-playing sources untouched.
    engine.SetSFXVolume(0.5f);
    EXPECT_NEAR(source->Volume, 0.4f, 0.0001f);

    // A refresh must re-derive from the request, not compound the last gain.
    engine.Console_RefreshAudio();
    EXPECT_NEAR(source->Volume, 0.4f, 0.0001f);

    engine.StopAllSounds();
    engine.Shutdown();
}

// ============================================================================
// Mixer buses actually applied (physics-audio-input-camera-07)
// ============================================================================

TEST(AudioEngineReal_MixerSoloSilencesUnrelatedBuses)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();

    // Nothing soloed: every bus is audible.
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("SFX"), 1.0f, 0.0001f);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Music"), 1.0f, 0.0001f);

    mixer.SetBusSolo("Music", true);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Music"), 1.0f, 0.0001f);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("SFX"), 0.0f, 0.0001f);
    // Master is an ancestor of the soloed bus, so it must still pass audio.
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Master"), 1.0f, 0.0001f);

    // Children of a soloed bus stay audible.
    mixer.CreateBus("Stingers", "Music");
    mixer.SetBusVolume("Stingers", 0.5f);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("Stingers"), 0.5f, 0.0001f);

    mixer.SetBusSolo("Music", false);
    EXPECT_NEAR(mixer.GetEffectiveBusVolume("SFX"), 1.0f, 0.0001f);
}

TEST(AudioEngineReal_MixerShutdownClearsState)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    mixer.SaveSnapshot("Gameplay");
    EXPECT_TRUE(mixer.GetBusNames().size() == 6u);

    mixer.Shutdown();
    EXPECT_TRUE(mixer.GetBusNames().empty());
    EXPECT_STR_CONTAINS(mixer.Console_GetStatus(), "Buses: 0");
    EXPECT_STR_CONTAINS(mixer.Console_GetStatus(), "Snapshots: 0");
}

// ============================================================================
// Occlusion honesty (physics-audio-input-camera-19)
// ============================================================================

TEST(AudioEngineReal_OcclusionReportsUnavailableWithoutPhysics)
{
    Spark::Audio::AudioMixer mixer;
    mixer.Initialize();
    mixer.SetOcclusionEnabled(true);

    // Enabled but unable to trace: callers must be able to tell this apart from
    // a measured clear line of sight.
    EXPECT_TRUE(mixer.IsOcclusionEnabled());
    EXPECT_FALSE(mixer.IsOcclusionAvailable());
    const auto result = mixer.CalculateOcclusion({0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f});
    EXPECT_EQ(result.wallCount, 0);
    EXPECT_NEAR(result.volumeScale, 1.0f, 0.0001f);
    EXPECT_STR_CONTAINS(mixer.Console_GetStatus(), "no physics attached");

    mixer.SetOcclusionEnabled(false);
    EXPECT_FALSE(mixer.IsOcclusionAvailable());
}

// ============================================================================
// Listener driven from the camera (physics-audio-input-camera-02)
// ============================================================================

TEST(AudioEngineReal_ListenerFromCameraDerivesVelocityAndRejectsTeleports)
{
    AudioEngine engine;

    engine.SetListenerFromCamera({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, 0.1f);
    EXPECT_NEAR(engine.GetListenerVelocity().z, 0.0f, 0.0001f);

    engine.SetListenerFromCamera({0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, 0.1f);
    EXPECT_NEAR(engine.GetListenerPosition().z, 1.0f, 0.0001f);
    EXPECT_NEAR(engine.GetListenerVelocity().z, 10.0f, 0.0001f);

    // A camera cut is not motion: faster than sound means "discontinuity".
    engine.SetListenerFromCamera({0.0f, 0.0f, 10000.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, 0.1f);
    EXPECT_NEAR(engine.GetListenerPosition().z, 10000.0f, 0.01f);
    EXPECT_NEAR(engine.GetListenerVelocity().z, 0.0f, 0.0001f);

    // Orientation is normalized so callers may pass an unnormalized basis.
    engine.SetListenerFromCamera({0.0f, 0.0f, 10000.0f}, {0.0f, 0.0f, 4.0f}, {0.0f, 3.0f, 0.0f}, 0.0f);
    const auto settings = engine.Console_GetSettings();
    EXPECT_NEAR(settings.listenerForward.z, 1.0f, 0.0001f);
    EXPECT_NEAR(settings.listenerUp.y, 1.0f, 0.0001f);

    engine.SetListenerVelocity({1.0f, 2.0f, 3.0f});
    EXPECT_NEAR(engine.GetListenerVelocity().y, 2.0f, 0.0001f);
}

// ============================================================================
// 3D attenuation range (physics-audio-input-camera-03)
// ============================================================================

TEST(AudioEngineReal_DistanceAttenuationHonorsAuthoredRange)
{
    // Inside the near range: full volume.
    EXPECT_NEAR(AudioEngine::ComputeDistanceAttenuation(0.0f, 1.0f, 50.0f, 1.0f), 1.0f, 0.0001f);
    EXPECT_NEAR(AudioEngine::ComputeDistanceAttenuation(1.0f, 1.0f, 50.0f, 1.0f), 1.0f, 0.0001f);

    // Beyond maxDistance the source is silent, not merely quiet.
    EXPECT_NEAR(AudioEngine::ComputeDistanceAttenuation(50.0f, 1.0f, 50.0f, 1.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(AudioEngine::ComputeDistanceAttenuation(500.0f, 1.0f, 50.0f, 1.0f), 0.0f, 0.0001f);

    // Monotonically decreasing in between.
    const float nearGain = AudioEngine::ComputeDistanceAttenuation(2.0f, 1.0f, 50.0f, 1.0f);
    const float midGain = AudioEngine::ComputeDistanceAttenuation(10.0f, 1.0f, 50.0f, 1.0f);
    EXPECT_LT(midGain, nearGain);
    EXPECT_GT(midGain, 0.0f);
    EXPECT_NEAR(nearGain, 0.5f, 0.0001f);

    // A wider authored range is louder at the same distance.
    EXPECT_GT(AudioEngine::ComputeDistanceAttenuation(10.0f, 5.0f, 50.0f, 1.0f), midGain);

    // A zero near range must not divide the source into silence.
    EXPECT_NEAR(AudioEngine::ComputeDistanceAttenuation(0.0f, 0.0f, 0.0f, 1.0f), 1.0f, 0.0001f);
    EXPECT_GT(AudioEngine::ComputeDistanceAttenuation(5.0f, 0.0f, 0.0f, 1.0f), 0.0f);
}

// ============================================================================
// Pooled-handle validity (physics-audio-input-camera-03)
// ============================================================================

TEST(AudioEngineReal_RecycledSourceInvalidatesAnOlderHandle)
{
    if (!AudioEngine::IsAudioBackendAvailable())
        SKIP_TEST("no audio backend on this platform (XAudio2 is Windows-only; stubs are no-ops)");

    AudioEngine engine;
    if (FAILED(engine.Initialize(1)))
    {
        EXPECT_FALSE(engine.IsAvailable());
        EXPECT_FALSE(engine.IsSourceLive(nullptr, 0u));
        SKIP_TEST("no XAudio2 output device: pooled-handle recycling cannot be exercised");
    }

    const std::wstring path = WriteSilentWav(L"spark_audio_handle_case", 44100, 1);
    ASSERT_FALSE(path.empty());
    ASSERT_TRUE(SUCCEEDED(engine.LoadSound("cue", path)));

    AudioSource* first = engine.PlaySound3D("cue", {1.0f, 0.0f, 0.0f});
    ASSERT_TRUE(first != nullptr);
    const uint32_t firstGeneration = first->Generation;
    EXPECT_TRUE(engine.IsSourceLive(first, firstGeneration));

    engine.StopSound(first);
    EXPECT_FALSE(engine.IsSourceLive(first, firstGeneration));

    // The one pooled slot is handed to a different owner: the old handle must
    // not silently start driving somebody else's playback.
    AudioSource* second = engine.PlaySound3D("cue", {9.0f, 0.0f, 0.0f});
    ASSERT_TRUE(second != nullptr);
    EXPECT_TRUE(second == first);
    EXPECT_TRUE(second->Generation != firstGeneration);
    EXPECT_FALSE(engine.IsSourceLive(first, firstGeneration));
    EXPECT_TRUE(engine.IsSourceLive(second, second->Generation));

    engine.StopAllSounds();
    engine.Shutdown();
}

// ============================================================================
// Device loss (physics-audio-input-camera-18)
// ============================================================================

TEST(AudioEngineReal_DeviceLossIsDetectableRatherThanSilentFailure)
{
    // XAUDIO2_E_DEVICE_INVALIDATED (0x88960004) is what every voice call returns
    // once the output device disappears; treating it as a generic failure is what
    // left the engine permanently silent while only logging warnings. The value is
    // spelled out because this test also compiles on Linux, where xaudio2.h and
    // its macro do not exist; the classifier under test is platform-neutral.
    constexpr HRESULT kXAudio2DeviceInvalidated = static_cast<HRESULT>(0x88960004L);
    EXPECT_TRUE(AudioEngine::IsDeviceLostResult(kXAudio2DeviceInvalidated));
    EXPECT_FALSE(AudioEngine::IsDeviceLostResult(static_cast<HRESULT>(0)));
    EXPECT_FALSE(AudioEngine::IsDeviceLostResult(static_cast<HRESULT>(0x80004005L)));
    // XAUDIO2_E_INVALID_CALL is a caller bug, not a vanished device: reporting it
    // as device loss would drop the engine into the recovery loop for good.
    EXPECT_FALSE(AudioEngine::IsDeviceLostResult(static_cast<HRESULT>(0x88960001L)));

    AudioEngine engine;
    // Before Initialize there is no mastering voice, so availability is false
    // and the backend must not report a constant "available".
    EXPECT_FALSE(engine.IsAvailable());
    EXPECT_FALSE(engine.IsDeviceLost());
    EXPECT_FALSE(engine.RecoverDevice());

    if (FAILED(engine.Initialize(1)))
    {
        EXPECT_FALSE(engine.IsAvailable());
        SKIP_TEST("no XAudio2 output device: device-loss recovery cannot be exercised");
    }

    EXPECT_TRUE(engine.IsAvailable());
    EXPECT_FALSE(engine.IsDeviceLost());
    // Recovery on a healthy engine rebuilds the mastering voice and stays usable.
    EXPECT_TRUE(engine.RecoverDevice());
    EXPECT_TRUE(engine.IsAvailable());

    engine.Shutdown();
    EXPECT_FALSE(engine.IsAvailable());
}
