/**
 * @file HRTFProcessor.cpp
 * @brief Analytical binaural HRTF renderer
 */

#include "HRTFProcessor.h"

#include <algorithm>
#include <cmath>

namespace Spark::Audio
{

    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        HRTFVec3 Sub(HRTFVec3 a, HRTFVec3 b)
        {
            return {a.x - b.x, a.y - b.y, a.z - b.z};
        }

        float Dot(HRTFVec3 a, HRTFVec3 b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        HRTFVec3 Cross(HRTFVec3 a, HRTFVec3 b)
        {
            return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }

        float Length(HRTFVec3 v)
        {
            return std::sqrt(Dot(v, v));
        }

        HRTFVec3 Normalize(HRTFVec3 v)
        {
            const float l = Length(v);
            if (l < 1e-6f)
                return {0.0f, 0.0f, 0.0f};
            return {v.x / l, v.y / l, v.z / l};
        }
    } // namespace

    bool HRTFProcessor::Initialize(uint32_t sampleRate, float headRadius)
    {
        m_sampleRate = std::max<uint32_t>(1u, sampleRate);
        m_headRadius = std::max(0.03f, headRadius);
        // Max possible delay corresponds to the Woodworth path difference at 90°.
        // path = r * (sinθ + θ) <= r * (1 + π/2); samples = path / c * sampleRate
        const float maxPath = m_headRadius * (1.0f + kPi * 0.5f);
        const size_t maxDelay = static_cast<size_t>(std::ceil(maxPath / m_speedOfSound * m_sampleRate)) + 4u;
        m_delayLine.assign(std::max<size_t>(32u, maxDelay * 2u), 0.0f);
        m_delayWrite = 0;
        m_lpState = 0.0f;
        m_lpAlpha = 1.0f;
        m_initialized = true;
        UpdateGeometry();
        m_currentLeftGain = m_targetLeftGain;
        m_currentRightGain = m_targetRightGain;
        return true;
    }

    void HRTFProcessor::Shutdown()
    {
        m_delayLine.clear();
        m_delayWrite = 0;
        m_initialized = false;
    }

    void HRTFProcessor::SetListener(HRTFVec3 position, HRTFVec3 forward, HRTFVec3 up)
    {
        m_listenerPos = position;
        m_listenerForward = Normalize(forward);
        m_listenerUp = Normalize(up);
        if (m_initialized)
            UpdateGeometry();
    }

    void HRTFProcessor::SetSource(HRTFVec3 position)
    {
        m_sourcePos = position;
        if (m_initialized)
            UpdateGeometry();
    }

    void HRTFProcessor::UpdateGeometry()
    {
        HRTFVec3 rel = Sub(m_sourcePos, m_listenerPos);
        const float dist = Length(rel);
        m_state.distance = dist;

        if (dist < 1e-5f)
        {
            m_targetLeftGain = m_targetRightGain = m_masterGain;
            m_targetItdSamples = 0;
            m_targetFarCutoff = 22000.0f;
            m_farEarIsLeft = false;
            m_state.azimuth = 0.0f;
            m_state.elevation = 0.0f;
            m_state.itdSamples = 0;
            m_state.leftGain = m_state.rightGain = m_masterGain;
            m_state.lowpassCutoff = m_targetFarCutoff;
            return;
        }

        const HRTFVec3 dir = Normalize(rel);
        // Left-handed (DirectXMath) convention: right = up × forward
        const HRTFVec3 right = Normalize(Cross(m_listenerUp, m_listenerForward));
        const float rightComp = Dot(dir, right);
        const float forwardComp = Dot(dir, m_listenerForward);
        const float upComp = Dot(dir, m_listenerUp);

        const float azimuth = std::atan2(rightComp, forwardComp); // [-π, π], +right
        const float elevation = std::asin(std::clamp(upComp, -1.0f, 1.0f));

        // --- ITD (Woodworth) ---
        const float absAz = std::abs(azimuth);
        const float halfAz = std::min(absAz, kPi - absAz); // fold behind the head
        const float pathDiff = m_headRadius * (std::sin(halfAz) + halfAz);
        const float itdSec = pathDiff / m_speedOfSound;
        const int32_t itdSamples = static_cast<int32_t>(std::round(itdSec * static_cast<float>(m_sampleRate)));

        // Positive azimuth = source on right; near ear is RIGHT → left is the FAR ear.
        m_farEarIsLeft = azimuth > 0.0f;

        // --- ILD (analytic shelf + cos² bias) ---
        // Near ear gets cos²(0) = 1, far ear gets cos²(azimuth / 2) rolloff.
        const float nearBias = 1.0f;
        const float farBias = std::cos(halfAz * 0.5f);
        const float farBias2 = farBias * farBias; // cos²
        // Gentle elevation shelf — above the head is ~0.9, below ~0.85.
        const float elevGain = 0.9f + 0.1f * std::sin(elevation);
        // Distance falloff (1/d, clamped)
        const float distFalloff = 1.0f / std::max(0.25f, dist);

        const float masterAndDistance = m_masterGain * distFalloff * elevGain;
        if (m_farEarIsLeft)
        {
            m_targetLeftGain = masterAndDistance * farBias2;
            m_targetRightGain = masterAndDistance * nearBias;
        }
        else
        {
            m_targetLeftGain = masterAndDistance * nearBias;
            m_targetRightGain = masterAndDistance * farBias2;
        }

        // --- Head-shadow low-pass cutoff for far ear ---
        // Full-band at 0°, ~1 kHz at 90°.
        const float cutoff = 22000.0f - (22000.0f - 1000.0f) * std::min(1.0f, halfAz / (kPi * 0.5f));
        m_targetFarCutoff = std::max(500.0f, cutoff);

        // One-pole alpha = 1 - exp(-2π fc / fs); clamp to sane range
        const float rc = 1.0f / (2.0f * kPi * m_targetFarCutoff);
        const float dt = 1.0f / static_cast<float>(m_sampleRate);
        m_lpAlpha = std::clamp(dt / (rc + dt), 0.0f, 1.0f);

        m_targetItdSamples = itdSamples;
        m_state.azimuth = azimuth;
        m_state.elevation = elevation;
        m_state.itdSamples = m_farEarIsLeft ? itdSamples : -itdSamples;
        m_state.leftGain = m_targetLeftGain;
        m_state.rightGain = m_targetRightGain;
        m_state.lowpassCutoff = m_targetFarCutoff;
    }

    void HRTFProcessor::PushDelay(float sample)
    {
        if (m_delayLine.empty())
            return;
        m_delayLine[m_delayWrite] = sample;
        m_delayWrite = (m_delayWrite + 1) % m_delayLine.size();
    }

    float HRTFProcessor::TapDelay(int32_t offsetSamples) const
    {
        if (m_delayLine.empty())
            return 0.0f;
        const int32_t size = static_cast<int32_t>(m_delayLine.size());
        int32_t idx = static_cast<int32_t>(m_delayWrite) - 1 - offsetSamples;
        idx = ((idx % size) + size) % size;
        return m_delayLine[static_cast<size_t>(idx)];
    }

    void HRTFProcessor::Process(const float* monoIn, float* stereoOut, size_t frameCount)
    {
        if (!m_initialized || !monoIn || !stereoOut || frameCount == 0)
            return;

        const float gainStep = 1.0f / static_cast<float>(frameCount);
        const float leftDelta = (m_targetLeftGain - m_currentLeftGain) * gainStep;
        const float rightDelta = (m_targetRightGain - m_currentRightGain) * gainStep;

        const int32_t itd = std::max(0, m_targetItdSamples);

        for (size_t i = 0; i < frameCount; ++i)
        {
            const float in = monoIn[i];
            PushDelay(in);
            const float nearSample = TapDelay(0);
            const float farSample = TapDelay(itd);

            // Head-shadow low-pass on the far ear
            m_lpState += m_lpAlpha * (farSample - m_lpState);
            const float farFiltered = m_lpState;

            m_currentLeftGain += leftDelta;
            m_currentRightGain += rightDelta;

            if (m_farEarIsLeft)
            {
                stereoOut[i * 2 + 0] = farFiltered * m_currentLeftGain;
                stereoOut[i * 2 + 1] = nearSample * m_currentRightGain;
            }
            else
            {
                stereoOut[i * 2 + 0] = nearSample * m_currentLeftGain;
                stereoOut[i * 2 + 1] = farFiltered * m_currentRightGain;
            }
        }

        // Snap smoothed values to final targets for next block.
        m_currentLeftGain = m_targetLeftGain;
        m_currentRightGain = m_targetRightGain;
    }

    std::vector<float> HRTFProcessor::Process(const std::vector<float>& monoIn)
    {
        std::vector<float> stereo(monoIn.size() * 2, 0.0f);
        Process(monoIn.data(), stereo.data(), monoIn.size());
        return stereo;
    }

} // namespace Spark::Audio
