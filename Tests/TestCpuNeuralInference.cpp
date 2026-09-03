/**
 * @file TestCpuNeuralInference.cpp
 * @brief Tests for SIMD-optimized CPU neural inference
 */

#include "TestFramework.h"
#include "Graphics/Neural/CpuNeuralInference.h"
#include "Graphics/Neural/NeuralInference.h"
#include "Graphics/Neural/NeuralTypes.h"

#include <cmath>
#include <numeric>
#include <vector>

using namespace Spark::Graphics::Neural;

// ============================================================================
// Initialization
// ============================================================================

TEST(CpuNeural_Initialize)
{
    CpuNeuralInference::Initialize();
    EXPECT_TRUE(CpuNeuralInference::IsInitialized());

    const char* isa = CpuNeuralInference::GetActiveISAName();
    ASSERT_TRUE(isa != nullptr);
    EXPECT_TRUE(std::string(isa).size() > 0);
}

// ============================================================================
// PrepareWeights
// ============================================================================

TEST(CpuNeural_PrepareWeightsPadding)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{5, 3, ActivationType::None}};

    // 5*3 weights + 3 biases = 18 params
    std::vector<float> weights(18, 1.0f);
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    EXPECT_EQ(layout.layerCount, 1u);
    EXPECT_EQ(layout.layers[0].inputSize, 5u);
    EXPECT_EQ(layout.layers[0].paddedInputSize, 8u); // 5 rounded up to 8
    EXPECT_EQ(layout.layers[0].outputSize, 3u);

    // Check that padding values are zero
    // Row 0: weights[0..4] = 1.0, weights[5..7] = 0.0
    uint32_t off = layout.layers[0].weightOffset;
    uint32_t padded = layout.layers[0].paddedInputSize;
    for (uint32_t row = 0; row < 3; ++row)
    {
        for (uint32_t col = 5; col < padded; ++col)
        {
            EXPECT_TRUE(layout.data[off + row * padded + col] == 0.0f);
        }
    }
}

TEST(CpuNeural_PrepareWeightsMultiLayer)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{3, 4, ActivationType::ReLU}, {4, 2, ActivationType::None}};

    uint32_t totalParams = desc.GetTotalParameters(); // 3*4+4 + 4*2+2 = 26
    std::vector<float> weights(totalParams, 0.5f);
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    EXPECT_EQ(layout.layerCount, 2u);
    EXPECT_EQ(layout.layers[0].paddedInputSize, 8u); // 3 -> 8
    EXPECT_EQ(layout.layers[1].paddedInputSize, 8u); // 4 -> 8
    EXPECT_EQ(layout.GetInputSize(), 3u);
    EXPECT_EQ(layout.GetOutputSize(), 2u);
}

// ============================================================================
// Single-layer evaluation
// ============================================================================

TEST(CpuNeural_LayerEvalIdentity)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{2, 2, ActivationType::None}};

    // Identity weights
    std::vector<float> weights = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    std::vector<float> input = {3.0f, 7.0f};
    std::vector<float> output(2, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, input.data(), output.data(), 1);

    EXPECT_TRUE(std::abs(output[0] - 3.0f) < 1e-5f);
    EXPECT_TRUE(std::abs(output[1] - 7.0f) < 1e-5f);
}

TEST(CpuNeural_LayerEvalReLU)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{2, 2, ActivationType::ReLU}};

    // output[0] = input[0] -> ReLU(1.0) = 1.0
    // output[1] = -input[0] -> ReLU(-1.0) = 0.0
    std::vector<float> weights = {1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    std::vector<float> input = {1.0f, 5.0f};
    std::vector<float> output(2, -1.0f);
    CpuNeuralInference::EvaluateBatch(layout, input.data(), output.data(), 1);

    EXPECT_TRUE(std::abs(output[0] - 1.0f) < 1e-5f);
    EXPECT_TRUE(std::abs(output[1] - 0.0f) < 1e-5f);
}

TEST(CpuNeural_LayerEvalSigmoid)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{1, 1, ActivationType::Sigmoid}};

    std::vector<float> weights = {1.0f, 0.0f}; // w=1, b=0
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    // sigmoid(0) = 0.5
    std::vector<float> input0 = {0.0f};
    std::vector<float> output0(1, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, input0.data(), output0.data(), 1);
    EXPECT_TRUE(std::abs(output0[0] - 0.5f) < 1e-5f);

    // sigmoid(10) ~ 1.0
    std::vector<float> inputLarge = {10.0f};
    std::vector<float> outputLarge(1, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, inputLarge.data(), outputLarge.data(), 1);
    EXPECT_TRUE(outputLarge[0] > 0.999f);

    // sigmoid(-10) ~ 0.0
    std::vector<float> inputNeg = {-10.0f};
    std::vector<float> outputNeg(1, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, inputNeg.data(), outputNeg.data(), 1);
    EXPECT_TRUE(outputNeg[0] < 0.001f);
}

TEST(CpuNeural_LayerEvalLeakyReLU)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{1, 1, ActivationType::LeakyReLU}};

    std::vector<float> weights = {1.0f, 0.0f};
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    // Positive: pass through
    std::vector<float> input1 = {5.0f};
    std::vector<float> output1(1);
    CpuNeuralInference::EvaluateBatch(layout, input1.data(), output1.data(), 1);
    EXPECT_TRUE(std::abs(output1[0] - 5.0f) < 1e-5f);

    // Negative: 0.01 * x
    std::vector<float> input2 = {-10.0f};
    std::vector<float> output2(1);
    CpuNeuralInference::EvaluateBatch(layout, input2.data(), output2.data(), 1);
    EXPECT_TRUE(std::abs(output2[0] - (-0.1f)) < 1e-5f);
}

TEST(CpuNeural_LayerEvalTanh)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{1, 1, ActivationType::Tanh}};

    std::vector<float> weights = {1.0f, 0.0f};
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    // tanh(0) = 0
    std::vector<float> input = {0.0f};
    std::vector<float> output(1);
    CpuNeuralInference::EvaluateBatch(layout, input.data(), output.data(), 1);
    EXPECT_TRUE(std::abs(output[0]) < 1e-5f);

    // tanh(2) ~ 0.9640
    std::vector<float> input2 = {2.0f};
    std::vector<float> output2(1);
    CpuNeuralInference::EvaluateBatch(layout, input2.data(), output2.data(), 1);
    EXPECT_TRUE(std::abs(output2[0] - std::tanh(2.0f)) < 1e-5f);
}

// ============================================================================
// Multi-layer evaluation
// ============================================================================

TEST(CpuNeural_MultiLayerNetwork)
{
    CpuNeuralInference::Initialize();

    // 2 -> 4 (ReLU) -> 1 (None): all weights=1, all biases=0
    // Layer 0: each output = 1+2 = 3, ReLU(3) = 3
    // Layer 1: output = 3*4 = 12
    NetworkDesc desc;
    desc.layers = {{2, 4, ActivationType::ReLU}, {4, 1, ActivationType::None}};

    std::vector<float> weights = {// Layer 0 weights (4 rows x 2 cols)
                                  1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                  // Layer 0 biases
                                  0.0f, 0.0f, 0.0f, 0.0f,
                                  // Layer 1 weights (1 row x 4 cols)
                                  1.0f, 1.0f, 1.0f, 1.0f,
                                  // Layer 1 bias
                                  0.0f};

    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    std::vector<float> input = {1.0f, 2.0f};
    std::vector<float> output(1, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, input.data(), output.data(), 1);

    EXPECT_TRUE(std::abs(output[0] - 12.0f) < 1e-4f);
}

TEST(CpuNeural_MatchesReferenceImpl)
{
    CpuNeuralInference::Initialize();

    // 2 -> 16 (ReLU) -> 16 (ReLU) -> 3 (Sigmoid)
    NetworkDesc desc;
    desc.name = "reference_match";
    desc.layers = {{2, 16, ActivationType::ReLU}, {16, 16, ActivationType::ReLU}, {16, 3, ActivationType::Sigmoid}};

    uint32_t totalParams = desc.GetTotalParameters();
    std::vector<float> weights(totalParams);
    for (uint32_t i = 0; i < totalParams; ++i)
    {
        weights[i] = 0.1f * std::sin(static_cast<float>(i) * 0.37f);
    }

    // SIMD path
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());
    std::vector<float> input = {0.5f, -0.3f};
    std::vector<float> simdOutput(3, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, input.data(), simdOutput.data(), 1);

    // Reference path (via NeuralInferenceEngine CPU fallback)
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();
    auto handle = engine.CreateNetwork(desc);
    engine.UploadWeights(handle, weights);

    std::vector<float> refOutput(3, 0.0f);
    engine.EvaluateCPU(handle, input.data(), refOutput.data(), 1);

    // Must match within floating-point tolerance
    for (int i = 0; i < 3; ++i)
    {
        EXPECT_TRUE(std::abs(simdOutput[i] - refOutput[i]) < 1e-4f);
    }

    engine.DestroyNetwork(handle);
}

// ============================================================================
// Batch evaluation
// ============================================================================

TEST(CpuNeural_BatchEvaluation)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{1, 1, ActivationType::None}};

    // w=2, b=1 -> output = 2*input + 1
    std::vector<float> weights = {2.0f, 1.0f};
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    constexpr uint32_t batchSize = 100;
    std::vector<float> input(batchSize);
    std::vector<float> output(batchSize, 0.0f);

    for (uint32_t i = 0; i < batchSize; ++i)
    {
        input[i] = static_cast<float>(i) * 0.1f;
    }

    CpuNeuralInference::EvaluateBatch(layout, input.data(), output.data(), batchSize);

    for (uint32_t i = 0; i < batchSize; ++i)
    {
        float expected = 2.0f * input[i] + 1.0f;
        EXPECT_TRUE(std::abs(output[i] - expected) < 1e-4f);
    }
}

TEST(CpuNeural_BatchParallel)
{
    CpuNeuralInference::Initialize();

    // Larger network to exercise parallel path
    NetworkDesc desc;
    desc.layers = {{4, 16, ActivationType::ReLU}, {16, 8, ActivationType::ReLU}, {8, 2, ActivationType::Sigmoid}};

    uint32_t totalParams = desc.GetTotalParameters();
    std::vector<float> weights(totalParams);
    for (uint32_t i = 0; i < totalParams; ++i)
    {
        weights[i] = 0.05f * std::sin(static_cast<float>(i) * 0.23f);
    }

    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    // Large batch to trigger ParallelFor (>= 16)
    constexpr uint32_t batchSize = 1000;
    std::vector<float> input(batchSize * 4);
    for (uint32_t i = 0; i < batchSize * 4; ++i)
    {
        input[i] = std::sin(static_cast<float>(i) * 0.17f);
    }

    std::vector<float> batchOutput(batchSize * 2, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, input.data(), batchOutput.data(), batchSize);

    // Verify by running a few samples individually
    for (uint32_t s = 0; s < batchSize; s += 100)
    {
        std::vector<float> singleOutput(2, 0.0f);
        CpuNeuralInference::EvaluateBatch(layout, input.data() + s * 4, singleOutput.data(), 1);

        EXPECT_TRUE(std::abs(batchOutput[s * 2 + 0] - singleOutput[0]) < 1e-5f);
        EXPECT_TRUE(std::abs(batchOutput[s * 2 + 1] - singleOutput[1]) < 1e-5f);
    }
}

// ============================================================================
// Edge cases
// ============================================================================

TEST(CpuNeural_EmptyBatch)
{
    CpuNeuralInference::Initialize();

    NetworkDesc desc;
    desc.layers = {{2, 2, ActivationType::None}};
    std::vector<float> weights = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    float output = -1.0f;
    CpuNeuralInference::EvaluateBatch(layout, nullptr, &output, 0);
    // Should not crash, output unchanged
    EXPECT_TRUE(output == -1.0f);
}

TEST(CpuNeural_LargeLayer)
{
    CpuNeuralInference::Initialize();

    // 64 -> 128 -> 64: exercises SIMD with larger vectors
    NetworkDesc desc;
    desc.layers = {{64, 128, ActivationType::ReLU}, {128, 64, ActivationType::None}};

    uint32_t totalParams = desc.GetTotalParameters();
    std::vector<float> weights(totalParams);
    for (uint32_t i = 0; i < totalParams; ++i)
    {
        weights[i] = 0.01f * std::cos(static_cast<float>(i) * 0.11f);
    }

    auto layout = CpuNeuralInference::PrepareWeights(desc, weights.data());

    std::vector<float> input(64, 1.0f);
    std::vector<float> output(64, 0.0f);
    CpuNeuralInference::EvaluateBatch(layout, input.data(), output.data(), 1);

    // Just verify it doesn't crash and produces finite results
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_TRUE(std::isfinite(output[i]));
    }
}
