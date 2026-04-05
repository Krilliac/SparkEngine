/**
 * @file NeuralPostProcessing.cpp
 * @brief Neural denoiser and super-resolution implementation (CPU paths)
 */

#include "NeuralPostProcessing.h"
#include "NeuralInference.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Spark::Graphics::Neural
{

    // =========================================================================
    // Neural Denoiser
    // =========================================================================

    bool NeuralDenoiser::Initialize(const DenoiserSettings& settings)
    {
        m_settings = settings;

        // Build denoiser MLP architecture
        // Input: 2 (patch pos) + 3 (color) + optional 3 (albedo) + optional 3 (normal)
        uint32_t inputSize = 5; // pos(2) + color(3)
        if (settings.useAlbedoGuide)
        {
            inputSize += 3;
        }
        if (settings.useNormalGuide)
        {
            inputSize += 3;
        }

        m_networkDesc.name = "neural_denoiser";
        m_networkDesc.layers.push_back({inputSize, 64, ActivationType::ReLU});
        m_networkDesc.layers.push_back({64, 64, ActivationType::ReLU});
        m_networkDesc.layers.push_back({64, 3, ActivationType::Sigmoid});

        m_initialized = true;
        return true;
    }

    void NeuralDenoiser::Shutdown()
    {
        if (m_mlpHandle.IsValid())
        {
            auto& engine = NeuralInferenceEngine::GetInstance();
            engine.DestroyNetwork(m_mlpHandle);
            m_mlpHandle = NetworkHandle{};
        }
        m_output.clear();
        m_initialized = false;
    }

    bool NeuralDenoiser::SetColorInput(const DenoiserBuffer& color)
    {
        m_colorInput = color;
        return true;
    }

    bool NeuralDenoiser::SetAlbedoGuide(const DenoiserBuffer& albedo)
    {
        m_albedoInput = albedo;
        return true;
    }

    bool NeuralDenoiser::SetNormalGuide(const DenoiserBuffer& normal)
    {
        m_normalInput = normal;
        return true;
    }

    bool NeuralDenoiser::LoadWeights(const std::vector<float>& weights)
    {
        if (!m_initialized)
        {
            return false;
        }

        auto& engine = NeuralInferenceEngine::GetInstance();

        if (m_mlpHandle.IsValid())
        {
            engine.DestroyNetwork(m_mlpHandle);
        }

        m_mlpHandle = engine.CreateNetwork(m_networkDesc);
        if (!m_mlpHandle.IsValid())
        {
            return false;
        }

        return engine.UploadWeights(m_mlpHandle, weights);
    }

    void NeuralDenoiser::DenoisePatch(const float* colorPatch, const float* albedoPatch, const float* normalPatch,
                                      float* outputPatch)
    {
        auto& engine = NeuralInferenceEngine::GetInstance();
        uint32_t inputSize = m_networkDesc.GetInputSize();

        for (uint32_t py = 0; py < kDenoisePatchSize; ++py)
        {
            for (uint32_t px = 0; px < kDenoisePatchSize; ++px)
            {
                uint32_t pixelIdx = py * kDenoisePatchSize + px;

                std::vector<float> mlpInput;
                mlpInput.reserve(inputSize);

                // Patch position
                mlpInput.push_back(static_cast<float>(px) / static_cast<float>(kDenoisePatchSize));
                mlpInput.push_back(static_cast<float>(py) / static_cast<float>(kDenoisePatchSize));

                // Noisy color
                mlpInput.push_back(colorPatch[pixelIdx * 3 + 0]);
                mlpInput.push_back(colorPatch[pixelIdx * 3 + 1]);
                mlpInput.push_back(colorPatch[pixelIdx * 3 + 2]);

                // Optional guides
                if (m_settings.useAlbedoGuide && albedoPatch)
                {
                    mlpInput.push_back(albedoPatch[pixelIdx * 3 + 0]);
                    mlpInput.push_back(albedoPatch[pixelIdx * 3 + 1]);
                    mlpInput.push_back(albedoPatch[pixelIdx * 3 + 2]);
                }
                if (m_settings.useNormalGuide && normalPatch)
                {
                    mlpInput.push_back(normalPatch[pixelIdx * 3 + 0]);
                    mlpInput.push_back(normalPatch[pixelIdx * 3 + 1]);
                    mlpInput.push_back(normalPatch[pixelIdx * 3 + 2]);
                }

                float output[3];
                engine.EvaluateCPU(m_mlpHandle, mlpInput.data(), output, 1);

                outputPatch[pixelIdx * 3 + 0] = output[0];
                outputPatch[pixelIdx * 3 + 1] = output[1];
                outputPatch[pixelIdx * 3 + 2] = output[2];
            }
        }
    }

    bool NeuralDenoiser::Execute()
    {
        if (!m_initialized || !m_colorInput.data)
        {
            return false;
        }

        auto startTime = std::chrono::steady_clock::now();

        uint32_t w = m_colorInput.width;
        uint32_t h = m_colorInput.height;
        m_output.resize(w * h * 3);

        // If no neural weights, fall back to pass-through
        if (!m_mlpHandle.IsValid())
        {
            std::memcpy(m_output.data(), m_colorInput.data, w * h * 3 * sizeof(float));
            return true;
        }

        uint32_t patchSize = kDenoisePatchSize;
        uint32_t patchesX = (w + patchSize - 1) / patchSize;
        uint32_t patchesY = (h + patchSize - 1) / patchSize;

        uint32_t patchPixels = patchSize * patchSize;
        std::vector<float> colorPatch(patchPixels * 3);
        std::vector<float> albedoPatch(patchPixels * 3);
        std::vector<float> normalPatch(patchPixels * 3);
        std::vector<float> outputPatch(patchPixels * 3);

        for (uint32_t py = 0; py < patchesY; ++py)
        {
            for (uint32_t px = 0; px < patchesX; ++px)
            {
                // Extract patch data
                for (uint32_t dy = 0; dy < patchSize; ++dy)
                {
                    for (uint32_t dx = 0; dx < patchSize; ++dx)
                    {
                        uint32_t sx = std::min(px * patchSize + dx, w - 1);
                        uint32_t sy = std::min(py * patchSize + dy, h - 1);
                        uint32_t srcIdx = (sy * w + sx) * 3;
                        uint32_t dstIdx = (dy * patchSize + dx) * 3;

                        colorPatch[dstIdx + 0] = m_colorInput.data[srcIdx + 0];
                        colorPatch[dstIdx + 1] = m_colorInput.data[srcIdx + 1];
                        colorPatch[dstIdx + 2] = m_colorInput.data[srcIdx + 2];

                        if (m_albedoInput.data)
                        {
                            albedoPatch[dstIdx + 0] = m_albedoInput.data[srcIdx + 0];
                            albedoPatch[dstIdx + 1] = m_albedoInput.data[srcIdx + 1];
                            albedoPatch[dstIdx + 2] = m_albedoInput.data[srcIdx + 2];
                        }
                        if (m_normalInput.data)
                        {
                            normalPatch[dstIdx + 0] = m_normalInput.data[srcIdx + 0];
                            normalPatch[dstIdx + 1] = m_normalInput.data[srcIdx + 1];
                            normalPatch[dstIdx + 2] = m_normalInput.data[srcIdx + 2];
                        }
                    }
                }

                DenoisePatch(colorPatch.data(), m_albedoInput.data ? albedoPatch.data() : nullptr,
                             m_normalInput.data ? normalPatch.data() : nullptr, outputPatch.data());

                // Write back to output
                for (uint32_t dy = 0; dy < patchSize; ++dy)
                {
                    for (uint32_t dx = 0; dx < patchSize; ++dx)
                    {
                        uint32_t dstX = px * patchSize + dx;
                        uint32_t dstY = py * patchSize + dy;
                        if (dstX >= w || dstY >= h)
                        {
                            continue;
                        }

                        uint32_t srcIdx = (dy * patchSize + dx) * 3;
                        uint32_t dstIdx = (dstY * w + dstX) * 3;
                        m_output[dstIdx + 0] = outputPatch[srcIdx + 0];
                        m_output[dstIdx + 1] = outputPatch[srcIdx + 1];
                        m_output[dstIdx + 2] = outputPatch[srcIdx + 2];
                    }
                }
            }
        }

        auto endTime = std::chrono::steady_clock::now();
        m_lastTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

        return true;
    }

    // =========================================================================
    // Neural Super-Resolution
    // =========================================================================

    bool NeuralSuperResolution::Initialize(const NeuralSRConfig& config)
    {
        m_config = config;

        // Input: 2 (sub-pixel pos) + 3 (center color) + optional 1 (depth) + 12 (4 neighbors * 3)
        uint32_t inputSize = 2 + 3 + 12;
        if (config.useDepthInput)
        {
            inputSize += 1;
        }

        m_networkDesc.name = "neural_sr";
        m_networkDesc.layers.push_back({inputSize, config.hiddenSize, ActivationType::ReLU});
        for (uint32_t i = 1; i < config.hiddenLayers; ++i)
        {
            m_networkDesc.layers.push_back({config.hiddenSize, config.hiddenSize, ActivationType::ReLU});
        }
        m_networkDesc.layers.push_back({config.hiddenSize, 3, ActivationType::Sigmoid});

        m_initialized = true;
        return true;
    }

    void NeuralSuperResolution::Shutdown()
    {
        if (m_mlpHandle.IsValid())
        {
            auto& engine = NeuralInferenceEngine::GetInstance();
            engine.DestroyNetwork(m_mlpHandle);
            m_mlpHandle = NetworkHandle{};
        }
        m_initialized = false;
    }

    bool NeuralSuperResolution::LoadWeights(const std::vector<float>& weights)
    {
        if (!m_initialized)
        {
            return false;
        }

        auto& engine = NeuralInferenceEngine::GetInstance();

        if (m_mlpHandle.IsValid())
        {
            engine.DestroyNetwork(m_mlpHandle);
        }

        m_mlpHandle = engine.CreateNetwork(m_networkDesc);
        if (!m_mlpHandle.IsValid())
        {
            return false;
        }

        return engine.UploadWeights(m_mlpHandle, weights);
    }

    void NeuralSuperResolution::UpscalePatch(const float* inputPatch, const float* depthPatch, float* outputPatch)
    {
        if (!m_mlpHandle.IsValid())
        {
            return;
        }

        auto& engine = NeuralInferenceEngine::GetInstance();
        uint32_t inputSize = m_networkDesc.GetInputSize();

        for (uint32_t oy = 0; oy < kSROutputPatchSize; ++oy)
        {
            for (uint32_t ox = 0; ox < kSROutputPatchSize; ++ox)
            {
                // Map output pixel to input coordinate
                float u = (static_cast<float>(ox) + 0.5f) / static_cast<float>(kSROutputPatchSize);
                float v = (static_cast<float>(oy) + 0.5f) / static_cast<float>(kSROutputPatchSize);
                float inX = u * kSRInputPatchSize - 0.5f;
                float inY = v * kSRInputPatchSize - 0.5f;

                // Clamp to input patch bounds
                int32_t cx = std::clamp(static_cast<int32_t>(inX), 0, static_cast<int32_t>(kSRInputPatchSize - 1));
                int32_t cy = std::clamp(static_cast<int32_t>(inY), 0, static_cast<int32_t>(kSRInputPatchSize - 1));

                auto sampleInput = [&](int32_t x, int32_t y) -> const float*
                {
                    x = std::clamp(x, 0, static_cast<int32_t>(kSRInputPatchSize - 1));
                    y = std::clamp(y, 0, static_cast<int32_t>(kSRInputPatchSize - 1));
                    return inputPatch + (y * kSRInputPatchSize + x) * 3;
                };

                const float* center = sampleInput(cx, cy);
                const float* left = sampleInput(cx - 1, cy);
                const float* right = sampleInput(cx + 1, cy);
                const float* up = sampleInput(cx, cy - 1);
                const float* down = sampleInput(cx, cy + 1);

                std::vector<float> mlpInput;
                mlpInput.reserve(inputSize);

                // Sub-pixel position
                mlpInput.push_back(static_cast<float>(ox) / static_cast<float>(kSROutputPatchSize));
                mlpInput.push_back(static_cast<float>(oy) / static_cast<float>(kSROutputPatchSize));

                // Center color
                mlpInput.push_back(center[0]);
                mlpInput.push_back(center[1]);
                mlpInput.push_back(center[2]);

                // Optional depth
                if (m_config.useDepthInput && depthPatch)
                {
                    mlpInput.push_back(depthPatch[cy * kSRInputPatchSize + cx]);
                }

                // Neighbor colors
                for (const float* n : {left, right, up, down})
                {
                    mlpInput.push_back(n[0]);
                    mlpInput.push_back(n[1]);
                    mlpInput.push_back(n[2]);
                }

                float output[3];
                engine.EvaluateCPU(m_mlpHandle, mlpInput.data(), output, 1);

                uint32_t outIdx = (oy * kSROutputPatchSize + ox) * 3;
                outputPatch[outIdx + 0] = output[0];
                outputPatch[outIdx + 1] = output[1];
                outputPatch[outIdx + 2] = output[2];
            }
        }

        m_patchesProcessed++;
    }

    std::vector<float> NeuralSuperResolution::UpscaleCPU(const float* input, const float* depth, uint32_t width,
                                                         uint32_t height)
    {
        if (!m_initialized || !m_mlpHandle.IsValid() || !input)
        {
            return {};
        }

        uint32_t outW = width * kSRScaleFactor;
        uint32_t outH = height * kSRScaleFactor;
        std::vector<float> result(outW * outH * 3, 0.0f);

        uint32_t patchesX = (width + kSRInputPatchSize - 1) / kSRInputPatchSize;
        uint32_t patchesY = (height + kSRInputPatchSize - 1) / kSRInputPatchSize;

        uint32_t inPatchPixels = kSRInputPatchSize * kSRInputPatchSize;
        uint32_t outPatchPixels = kSROutputPatchSize * kSROutputPatchSize;

        std::vector<float> inputPatch(inPatchPixels * 3);
        std::vector<float> depthPatch(inPatchPixels);
        std::vector<float> outputPatch(outPatchPixels * 3);

        for (uint32_t py = 0; py < patchesY; ++py)
        {
            for (uint32_t px = 0; px < patchesX; ++px)
            {
                // Extract input patch
                for (uint32_t dy = 0; dy < kSRInputPatchSize; ++dy)
                {
                    for (uint32_t dx = 0; dx < kSRInputPatchSize; ++dx)
                    {
                        uint32_t sx = std::min(px * kSRInputPatchSize + dx, width - 1);
                        uint32_t sy = std::min(py * kSRInputPatchSize + dy, height - 1);
                        uint32_t srcIdx = (sy * width + sx) * 3;
                        uint32_t dstIdx = (dy * kSRInputPatchSize + dx) * 3;

                        inputPatch[dstIdx + 0] = input[srcIdx + 0];
                        inputPatch[dstIdx + 1] = input[srcIdx + 1];
                        inputPatch[dstIdx + 2] = input[srcIdx + 2];

                        if (depth)
                        {
                            depthPatch[dy * kSRInputPatchSize + dx] = depth[sy * width + sx];
                        }
                    }
                }

                UpscalePatch(inputPatch.data(), depth ? depthPatch.data() : nullptr, outputPatch.data());

                // Write output patch
                for (uint32_t dy = 0; dy < kSROutputPatchSize; ++dy)
                {
                    for (uint32_t dx = 0; dx < kSROutputPatchSize; ++dx)
                    {
                        uint32_t dstX = px * kSROutputPatchSize + dx;
                        uint32_t dstY = py * kSROutputPatchSize + dy;
                        if (dstX >= outW || dstY >= outH)
                        {
                            continue;
                        }

                        uint32_t srcIdx = (dy * kSROutputPatchSize + dx) * 3;
                        uint32_t dstIdx = (dstY * outW + dstX) * 3;
                        result[dstIdx + 0] = outputPatch[srcIdx + 0];
                        result[dstIdx + 1] = outputPatch[srcIdx + 1];
                        result[dstIdx + 2] = outputPatch[srcIdx + 2];
                    }
                }
            }
        }

        return result;
    }

    std::string NeuralSuperResolution::Console_GetStatus() const
    {
        std::string status = "Neural Super-Resolution:\n";
        status += "  Initialized: " + std::string(m_initialized ? "YES" : "NO") + "\n";
        status += "  Weights loaded: " + std::string(m_mlpHandle.IsValid() ? "YES" : "NO") + "\n";
        status += "  Patches processed: " + std::to_string(m_patchesProcessed) + "\n";
        status += "  Scale factor: " + std::to_string(kSRScaleFactor) + "x\n";
        return status;
    }

} // namespace Spark::Graphics::Neural
