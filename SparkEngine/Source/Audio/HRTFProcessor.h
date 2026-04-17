/**
 * @file HRTFProcessor.h
 * @brief Lightweight binaural HRTF approximation (ITD + ILD + head shadow)
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a dependency-free binaural renderer for 3D-positioned audio. The
 * processor implements the two dominant cues from the head-related transfer
 * function analytically:
 *
 *  - **Interaural Time Difference (ITD)**: the far ear receives the signal
 *    delayed by `headRadius / speedOfSound * (sinθ + θ)` (Woodworth formula).
 *    Expressed in samples at the current sample rate.
 *  - **Interaural Level Difference (ILD)**: frequency-dependent shadowing of
 *    the far ear approximated by a first-order one-pole low-pass whose cutoff
 *    shrinks with azimuth, plus a cos²(θ/2) gain bias.
 *  - **Elevation cue**: a mild pinna shelf filter biases brightness upward
 *    with elevation.
 *
 * A full measured HRIR database can be swapped in later — the interface
 * `Process()` takes mono frames and emits interleaved stereo so the rest of
 * the pipeline is unaffected.
 *
 * The processor is fully portable — no platform audio SDKs are required.
 *
 * ## Usage
 * @code
 *   HRTFProcessor hrtf;
 *   hrtf.Initialize(48000);
 *   hrtf.SetListener({0,0,0}, {0,0,1}, {0,1,0});
 *   hrtf.SetSource({5, 0, 2});
 *   std::array<float, 1024> mono{};
 *   std::array<float, 2048> stereo{};
 *   hrtf.Process(mono.data(), stereo.data(), mono.size());
 * @endcode
 */

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace Spark::Audio
{

    /**
     * @brief Simple 3-component vector used by the HRTF processor.
     */
    struct HRTFVec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /**
     * @brief Diagnostic snapshot of the last Process() call.
     */
    struct HRTFState
    {
        float azimuth = 0.0f;   ///< Radians, right ear positive
        float elevation = 0.0f; ///< Radians, up positive
        float distance = 0.0f;  ///< Metres from listener to source
        int32_t itdSamples = 0; ///< Delay between ears (positive = right leads)
        float leftGain = 1.0f;  ///< Final per-ear gain (post ILD)
        float rightGain = 1.0f;
        float lowpassCutoff = 22000.0f; ///< Cutoff applied to far ear (Hz)
    };

    /**
     * @brief Portable analytical HRTF binaural renderer.
     *
     * Operates on mono PCM float samples in [-1, 1] and emits interleaved
     * stereo (L0, R0, L1, R1, ...). No dynamic allocation after Initialize()
     * except when the ITD delay line grows for new larger azimuths.
     */
    class HRTFProcessor
    {
      public:
        HRTFProcessor() = default;
        ~HRTFProcessor() = default;

        HRTFProcessor(const HRTFProcessor&) = delete;
        HRTFProcessor& operator=(const HRTFProcessor&) = delete;

        /**
         * @brief Allocate the delay lines and filter state for a sample rate.
         * @param sampleRate   Samples per second (e.g. 48000)
         * @param headRadius   Head radius in metres (default ~8.75 cm)
         * @return True on success
         */
        bool Initialize(uint32_t sampleRate, float headRadius = 0.0875f);

        /// @brief Release internal buffers
        void Shutdown();

        /// @brief Set the listener pose (forward and up must be normalized)
        void SetListener(HRTFVec3 position, HRTFVec3 forward, HRTFVec3 up);

        /// @brief Set the world-space source position
        void SetSource(HRTFVec3 position);

        /// @brief Per-source gain multiplier applied after ITD/ILD
        void SetMasterGain(float gain) { m_masterGain = gain; }

        /**
         * @brief Render a block of mono samples to interleaved stereo.
         *
         * Safe to call on different source positions between blocks; the
         * processor cross-fades gains over the block to avoid zipper noise.
         *
         * @param monoIn          Pointer to @p frameCount mono samples
         * @param stereoOut       Pointer to @p frameCount * 2 interleaved samples
         * @param frameCount      Number of frames (not samples) to process
         */
        void Process(const float* monoIn, float* stereoOut, size_t frameCount);

        /// @brief Convenience helper — allocates and returns a stereo vector
        std::vector<float> Process(const std::vector<float>& monoIn);

        /// @brief Diagnostic snapshot from the last Process call
        const HRTFState& GetLastState() const { return m_state; }

        bool IsInitialized() const { return m_initialized; }
        uint32_t GetSampleRate() const { return m_sampleRate; }

      private:
        void UpdateGeometry();
        void PushDelay(float sample);
        float TapDelay(int32_t offsetSamples) const;

        uint32_t m_sampleRate = 48000;
        float m_headRadius = 0.0875f;
        float m_speedOfSound = 343.0f;
        float m_masterGain = 1.0f;

        HRTFVec3 m_listenerPos{};
        HRTFVec3 m_listenerForward{0.0f, 0.0f, 1.0f};
        HRTFVec3 m_listenerUp{0.0f, 1.0f, 0.0f};
        HRTFVec3 m_sourcePos{0.0f, 0.0f, 1.0f};

        // Per-frame updated quantities (reset by UpdateGeometry())
        float m_targetLeftGain = 1.0f;
        float m_targetRightGain = 1.0f;
        int32_t m_targetItdSamples = 0;
        float m_targetFarCutoff = 22000.0f;
        bool m_farEarIsLeft = false; // else right

        // Smoothed state (interpolated per block)
        float m_currentLeftGain = 1.0f;
        float m_currentRightGain = 1.0f;

        // One-pole low-pass for the far ear ("head-shadow" shelf)
        float m_lpAlpha = 1.0f;
        float m_lpState = 0.0f;

        // Delay line for ITD — size >= max expected delay in samples
        std::vector<float> m_delayLine;
        size_t m_delayWrite = 0;

        HRTFState m_state{};
        bool m_initialized = false;
    };

} // namespace Spark::Audio
