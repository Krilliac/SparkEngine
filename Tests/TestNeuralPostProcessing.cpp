/**
 * @file TestNeuralPostProcessing.cpp
 * @brief Tests for neural denoiser and super-resolution
 */

#include "TestFramework.h"
#include "Graphics/Neural/NeuralPostProcessing.h"
#include "Graphics/Neural/NeuralInference.h"

#include <cmath>
#include <vector>

using namespace Spark::Graphics;
using namespace Spark::Graphics::Neural;

// ============================================================================
// Neural Denoiser tests
// ============================================================================

TEST(NeuralDenoise_Initialize)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralDenoiser denoiser;
    DenoiserSettings settings;
    settings.useAlbedoGuide = false;
    settings.useNormalGuide = false;

    EXPECT_TRUE(denoiser.Initialize(settings));
    EXPECT_EQ(static_cast<int>(denoiser.GetBackend()), static_cast<int>(DenoiserBackend::Neural));

    denoiser.Shutdown();
}

TEST(NeuralDenoise_PassThroughWithoutWeights)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralDenoiser denoiser;
    DenoiserSettings settings;
    settings.useAlbedoGuide = false;
    settings.useNormalGuide = false;
    denoiser.Initialize(settings);

    // Without weights loaded, Execute should pass through input
    constexpr uint32_t w = 8;
    constexpr uint32_t h = 8;
    std::vector<float> colorData(w * h * 3, 0.5f);

    DenoiserBuffer colorBuf;
    colorBuf.data = colorData.data();
    colorBuf.width = w;
    colorBuf.height = h;
    colorBuf.channels = 3;

    denoiser.SetColorInput(colorBuf);
    EXPECT_TRUE(denoiser.Execute());

    const float* output = denoiser.GetOutput();
    EXPECT_TRUE(output != nullptr);
    EXPECT_EQ(denoiser.GetOutputWidth(), w);
    EXPECT_EQ(denoiser.GetOutputHeight(), h);

    // Output should match input (pass-through when no weights)
    for (uint32_t i = 0; i < w * h * 3; ++i)
    {
        EXPECT_TRUE(std::abs(output[i] - 0.5f) < 1e-5f);
    }

    denoiser.Shutdown();
}

TEST(NeuralDenoise_WithWeightsProducesOutput)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralDenoiser denoiser;
    DenoiserSettings settings;
    settings.useAlbedoGuide = false;
    settings.useNormalGuide = false;
    denoiser.Initialize(settings);

    // Create random weights matching the network architecture
    // Input: 5 (pos(2) + color(3)), Hidden: 64, 64, Output: 3
    // Layer 0: 5*64 + 64 = 384
    // Layer 1: 64*64 + 64 = 4160
    // Layer 2: 64*3 + 3 = 195
    // Total: 4739
    uint32_t totalParams = 5 * 64 + 64 + 64 * 64 + 64 + 64 * 3 + 3;
    std::vector<float> weights(totalParams, 0.01f);
    EXPECT_TRUE(denoiser.LoadWeights(weights));
    EXPECT_TRUE(denoiser.HasWeights());

    constexpr uint32_t w = 8;
    constexpr uint32_t h = 8;
    std::vector<float> colorData(w * h * 3, 0.5f);

    DenoiserBuffer colorBuf;
    colorBuf.data = colorData.data();
    colorBuf.width = w;
    colorBuf.height = h;
    colorBuf.channels = 3;
    denoiser.SetColorInput(colorBuf);

    EXPECT_TRUE(denoiser.Execute());

    const float* output = denoiser.GetOutput();
    ASSERT_TRUE(output != nullptr);

    // With sigmoid output, values should be in [0, 1]
    for (uint32_t i = 0; i < w * h * 3; ++i)
    {
        EXPECT_TRUE(output[i] >= 0.0f && output[i] <= 1.0f);
    }

    EXPECT_TRUE(denoiser.GetLastExecutionTimeMs() >= 0.0f);

    denoiser.Shutdown();
}

TEST(NeuralDenoise_NullInputRejected)
{
    NeuralDenoiser denoiser;
    DenoiserSettings settings;
    denoiser.Initialize(settings);

    // No color input set — Execute should fail
    EXPECT_FALSE(denoiser.Execute());

    denoiser.Shutdown();
}

// ============================================================================
// Neural Super-Resolution tests
// ============================================================================

TEST(NeuralSR_Initialize)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralSuperResolution sr;
    NeuralSRConfig config;
    config.hiddenSize = 32;
    config.hiddenLayers = 2;
    config.useDepthInput = false;

    EXPECT_TRUE(sr.Initialize(config));
    EXPECT_TRUE(sr.IsInitialized());

    sr.Shutdown();
    EXPECT_FALSE(sr.IsInitialized());
}

TEST(NeuralSR_UpscaleWithWeights)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralSuperResolution sr;
    NeuralSRConfig config;
    config.hiddenSize = 16;
    config.hiddenLayers = 1;
    config.useDepthInput = false;

    sr.Initialize(config);

    // Input: 2 (pos) + 3 (center) + 12 (neighbors) = 17
    // Layer 0: 17*16 + 16 = 288
    // Layer 1: 16*3 + 3 = 51
    // Total: 339
    uint32_t totalParams = 17 * 16 + 16 + 16 * 3 + 3;
    std::vector<float> weights(totalParams, 0.01f);
    EXPECT_TRUE(sr.LoadWeights(weights));

    // Small 8x8 input image
    constexpr uint32_t inW = 8;
    constexpr uint32_t inH = 8;
    std::vector<float> input(inW * inH * 3, 0.5f);

    auto output = sr.UpscaleCPU(input.data(), nullptr, inW, inH);

    // Output should be 16x16
    EXPECT_EQ(output.size(), static_cast<size_t>(16 * 16 * 3));

    // All values should be in [0, 1] (sigmoid output)
    for (float val : output)
    {
        EXPECT_TRUE(val >= 0.0f && val <= 1.0f);
    }

    sr.Shutdown();
}

TEST(NeuralSR_WithDepthInput)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralSuperResolution sr;
    NeuralSRConfig config;
    config.hiddenSize = 16;
    config.hiddenLayers = 1;
    config.useDepthInput = true;

    sr.Initialize(config);

    // Input: 2 + 3 + 1 (depth) + 12 = 18
    uint32_t totalParams = 18 * 16 + 16 + 16 * 3 + 3;
    std::vector<float> weights(totalParams, 0.01f);
    sr.LoadWeights(weights);

    constexpr uint32_t inW = 8;
    constexpr uint32_t inH = 8;
    std::vector<float> input(inW * inH * 3, 0.5f);
    std::vector<float> depth(inW * inH, 0.9f);

    auto output = sr.UpscaleCPU(input.data(), depth.data(), inW, inH);
    EXPECT_EQ(output.size(), static_cast<size_t>(16 * 16 * 3));

    sr.Shutdown();
}

TEST(NeuralSR_NullInputReturnsEmpty)
{
    NeuralSuperResolution sr;
    NeuralSRConfig config;
    config.hiddenSize = 16;
    config.hiddenLayers = 1;
    sr.Initialize(config);

    auto output = sr.UpscaleCPU(nullptr, nullptr, 8, 8);
    EXPECT_TRUE(output.empty());

    sr.Shutdown();
}

TEST(NeuralSR_ConsoleStatus)
{
    NeuralSuperResolution sr;
    NeuralSRConfig config;
    sr.Initialize(config);

    auto status = sr.Console_GetStatus();
    EXPECT_TRUE(status.find("Neural Super-Resolution") != std::string::npos);
    EXPECT_TRUE(status.find("2x") != std::string::npos);

    sr.Shutdown();
}
