/**
 * @file TestHRTFProcessor.cpp
 * @brief Tests for the binaural HRTF processor
 *
 * Covers the analytical ITD/ILD cues: sources to the right/left/front/back,
 * zero-length ray handling, and output block shape. Uses the real
 * HRTFProcessor class (no standalone reimpl).
 */

#include "TestFramework.h"
#include "Audio/HRTFProcessor.h"

#include <cmath>
#include <vector>

using Spark::Audio::HRTFProcessor;
using Spark::Audio::HRTFState;
using Spark::Audio::HRTFVec3;

namespace
{
    std::vector<float> MakeTone(uint32_t sampleRate, float frequency, size_t frames)
    {
        std::vector<float> out(frames);
        const float tau = 6.28318530718f;
        for (size_t i = 0; i < frames; ++i)
        {
            out[i] = std::sin(tau * frequency * static_cast<float>(i) / static_cast<float>(sampleRate));
        }
        return out;
    }

    float Rms(const float* buf, size_t count, size_t stride, size_t offset)
    {
        if (count == 0)
            return 0.0f;
        double acc = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const float v = buf[i * stride + offset];
            acc += static_cast<double>(v) * static_cast<double>(v);
        }
        return static_cast<float>(std::sqrt(acc / static_cast<double>(count)));
    }
} // namespace

TEST(HRTF_InitializeSucceeds)
{
    HRTFProcessor p;
    EXPECT_FALSE(p.IsInitialized());
    EXPECT_TRUE(p.Initialize(48000));
    EXPECT_TRUE(p.IsInitialized());
    EXPECT_EQ(p.GetSampleRate(), 48000u);
    p.Shutdown();
    EXPECT_FALSE(p.IsInitialized());
}

TEST(HRTF_SourceFrontIsSymmetric)
{
    HRTFProcessor p;
    p.Initialize(48000);
    p.SetListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    p.SetSource({0, 0, 5}); // directly in front

    auto mono = MakeTone(48000, 440.0f, 512);
    std::vector<float> stereo(mono.size() * 2, 0.0f);
    p.Process(mono.data(), stereo.data(), mono.size());

    const HRTFState& state = p.GetLastState();
    EXPECT_NEAR(state.azimuth, 0.0f, 0.01f);
    EXPECT_EQ(state.itdSamples, 0);

    const float l = Rms(stereo.data(), mono.size(), 2, 0);
    const float r = Rms(stereo.data(), mono.size(), 2, 1);
    EXPECT_NEAR(l, r, 0.05f);
}

TEST(HRTF_SourceRightLouderOnRight)
{
    HRTFProcessor p;
    p.Initialize(48000);
    p.SetListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    p.SetSource({5, 0, 0}); // directly to the right

    auto mono = MakeTone(48000, 440.0f, 1024);
    std::vector<float> stereo(mono.size() * 2, 0.0f);
    p.Process(mono.data(), stereo.data(), mono.size());

    const HRTFState& state = p.GetLastState();
    EXPECT_GT(state.azimuth, 0.0f);
    EXPECT_GT(state.rightGain, state.leftGain);

    const float l = Rms(stereo.data(), mono.size(), 2, 0);
    const float r = Rms(stereo.data(), mono.size(), 2, 1);
    EXPECT_GT(r, l);
}

TEST(HRTF_SourceLeftLouderOnLeft)
{
    HRTFProcessor p;
    p.Initialize(48000);
    p.SetListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    p.SetSource({-5, 0, 0});

    auto mono = MakeTone(48000, 440.0f, 1024);
    std::vector<float> stereo(mono.size() * 2, 0.0f);
    p.Process(mono.data(), stereo.data(), mono.size());

    const HRTFState& state = p.GetLastState();
    EXPECT_LT(state.azimuth, 0.0f);
    EXPECT_GT(state.leftGain, state.rightGain);
}

TEST(HRTF_ITDPositiveForRightSource)
{
    HRTFProcessor p;
    p.Initialize(48000);
    p.SetListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0});
    p.SetSource({5, 0, 0});
    // Process one block so geometry is refreshed (UpdateGeometry runs in setters)
    std::vector<float> mono(128, 0.0f);
    std::vector<float> stereo(mono.size() * 2, 0.0f);
    p.Process(mono.data(), stereo.data(), mono.size());
    const HRTFState& state = p.GetLastState();
    // Woodworth: path diff at 90° ≈ r * (1 + π/2) ≈ 0.224 m → ~31 samples at 48 kHz
    EXPECT_GT(state.itdSamples, 0);
    EXPECT_LT(state.itdSamples, 80);
}

TEST(HRTF_ZeroSourcePositionNoCrash)
{
    HRTFProcessor p;
    p.Initialize(48000);
    p.SetSource({0, 0, 0}); // same as listener
    std::vector<float> mono(64, 1.0f);
    std::vector<float> stereo(mono.size() * 2, 0.0f);
    p.Process(mono.data(), stereo.data(), mono.size());
    // Expect no NaN / inf
    for (float v : stereo)
    {
        EXPECT_TRUE(std::isfinite(v));
    }
}

TEST(HRTF_ProcessVectorConvenience)
{
    HRTFProcessor p;
    p.Initialize(48000);
    p.SetSource({3, 0, 3});
    auto mono = MakeTone(48000, 880.0f, 256);
    auto stereo = p.Process(mono);
    EXPECT_EQ(stereo.size(), mono.size() * 2);
}

TEST(HRTF_DistanceFalloffAttenuates)
{
    HRTFProcessor p;
    p.Initialize(48000);
    p.SetListener({0, 0, 0}, {0, 0, 1}, {0, 1, 0});

    p.SetSource({2, 0, 0});
    std::vector<float> mono(256, 0.0f);
    std::vector<float> stereoNear(mono.size() * 2, 0.0f);
    p.Process(mono.data(), stereoNear.data(), mono.size());
    const float nearGain = p.GetLastState().rightGain;

    p.SetSource({20, 0, 0});
    std::vector<float> stereoFar(mono.size() * 2, 0.0f);
    p.Process(mono.data(), stereoFar.data(), mono.size());
    const float farGain = p.GetLastState().rightGain;

    EXPECT_GT(nearGain, farGain);
}
