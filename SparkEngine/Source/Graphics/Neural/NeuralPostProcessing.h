/**
 * @file NeuralPostProcessing.h
 * @brief Neural-enhanced denoiser and super-resolution post-processing
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a neural denoiser (implements IDenoiser) and neural super-
 * resolution enhancement. Both use patch-based MLP evaluation via the
 * NeuralInferenceEngine, replacing the bilateral-filter software fallback
 * with learned image processing.
 *
 * ## Denoiser
 * Processes 8x8 pixel patches through a small MLP:
 *   Input:  noisy color (3*8*8) + optional albedo/normal guides
 *   Output: denoised color (3*8*8)
 *
 * ## Super-Resolution
 * Enhances SparkSR's temporal upscaler with a neural refinement pass:
 *   Input:  8x8 low-res patch (3*8*8) + depth (8*8)
 *   Output: 16x16 high-res patch (3*16*16)
 *
 * @see DenoiserInterface.h, UpscalingSystem.h, NeuralInference.h
 */

#pragma once

#include "NeuralTypes.h"
#include "Graphics/DenoiserInterface.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics::Neural
{

    /** @brief Patch size for neural denoiser input (8x8 pixels). */
    static constexpr uint32_t kDenoisePatchSize = 8;

    /** @brief Patch size for neural SR input (8x8 pixels → 16x16 output). */
    static constexpr uint32_t kSRInputPatchSize = 8;
    static constexpr uint32_t kSROutputPatchSize = 16;
    static constexpr uint32_t kSRScaleFactor = 2;

    // =========================================================================
    // Neural Denoiser
    // =========================================================================

    /**
     * @brief Neural-network-based image denoiser.
     *
     * Implements IDenoiser with a learned MLP that processes 8x8 patches.
     * Falls back to the base SoftwareDenoiser if weights are not loaded.
     */
    class NeuralDenoiser : public IDenoiser
    {
      public:
        bool Initialize(const DenoiserSettings& settings) override;
        void Shutdown() override;

        bool SetColorInput(const DenoiserBuffer& color) override;
        bool SetAlbedoGuide(const DenoiserBuffer& albedo) override;
        bool SetNormalGuide(const DenoiserBuffer& normal) override;

        bool Execute() override;

        const float* GetOutput() const override { return m_output.data(); }
        uint32_t GetOutputWidth() const override { return m_colorInput.width; }
        uint32_t GetOutputHeight() const override { return m_colorInput.height; }
        DenoiserBackend GetBackend() const override { return DenoiserBackend::Neural; }
        float GetLastExecutionTimeMs() const override { return m_lastTimeMs; }

        /** @brief Load pre-trained denoiser weights. */
        bool LoadWeights(const std::vector<float>& weights);

        /** @brief Check if neural weights are loaded. */
        bool HasWeights() const { return m_mlpHandle.IsValid(); }

      private:
        /** @brief Process a single 8x8 patch through the MLP. */
        void DenoisePatch(const float* colorPatch, const float* albedoPatch, const float* normalPatch,
                          float* outputPatch);

        DenoiserSettings m_settings;
        DenoiserBuffer m_colorInput;
        DenoiserBuffer m_albedoInput;
        DenoiserBuffer m_normalInput;
        std::vector<float> m_output;
        float m_lastTimeMs = 0.0f;
        bool m_initialized = false;

        NetworkDesc m_networkDesc;
        NetworkHandle m_mlpHandle;
    };

    // =========================================================================
    // Neural Super-Resolution
    // =========================================================================

    /** @brief Configuration for neural super-resolution. */
    struct NeuralSRConfig
    {
        uint32_t hiddenSize = 128; ///< Neurons per hidden layer
        uint32_t hiddenLayers = 3; ///< Number of hidden layers
        bool useDepthInput = true; ///< Include depth buffer as input
    };

    /**
     * @brief Neural super-resolution post-processing pass.
     *
     * Upscales 8x8 patches to 16x16 using a learned MLP. Designed as
     * an optional enhancement pass after SparkSR temporal accumulation.
     */
    class NeuralSuperResolution
    {
      public:
        NeuralSuperResolution() = default;

        /**
         * @brief Initialize the neural SR system.
         * @param config Configuration
         * @return true on success
         */
        bool Initialize(const NeuralSRConfig& config = {});

        /** @brief Shut down and free resources. */
        void Shutdown();

        /** @brief Check if initialized. */
        bool IsInitialized() const { return m_initialized; }

        /** @brief Load pre-trained SR weights. */
        bool LoadWeights(const std::vector<float>& weights);

        /**
         * @brief Upscale an image using neural SR (CPU path).
         * @param input Low-res RGB float image (width * height * 3)
         * @param depth Optional depth buffer (width * height floats, nullptr to skip)
         * @param width Input width
         * @param height Input height
         * @return Upscaled RGB float image (width*2 * height*2 * 3)
         */
        std::vector<float> UpscaleCPU(const float* input, const float* depth, uint32_t width, uint32_t height);

        /** @brief Console status string. */
        std::string Console_GetStatus() const;

      private:
        /** @brief Process a single patch through the SR MLP. */
        void UpscalePatch(const float* inputPatch, const float* depthPatch, float* outputPatch);

        bool m_initialized = false;
        NeuralSRConfig m_config;
        NetworkDesc m_networkDesc;
        NetworkHandle m_mlpHandle;
        uint32_t m_patchesProcessed = 0;
    };

} // namespace Spark::Graphics::Neural
