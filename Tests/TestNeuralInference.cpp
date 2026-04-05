/**
 * @file TestNeuralInference.cpp
 * @brief Tests for neural inference engine (CPU path)
 */

#include "TestFramework.h"
#include "Graphics/Neural/NeuralTypes.h"
#include "Graphics/Neural/NeuralInference.h"
#include "Graphics/Neural/NeuralWeights.h"

#include <cmath>
#include <vector>

using namespace Spark::Graphics::Neural;

// ============================================================================
// NeuralTypes tests
// ============================================================================

TEST(Neural_NetworkDescTotalParams)
{
    NetworkDesc desc;
    desc.layers = {{2, 16, ActivationType::ReLU}, {16, 4, ActivationType::None}};
    // Layer 0: 2*16 + 16 = 48
    // Layer 1: 16*4 + 4  = 68
    EXPECT_EQ(desc.GetTotalParameters(), static_cast<uint32_t>(116));
}

TEST(Neural_NetworkDescEmptyLayers)
{
    NetworkDesc desc;
    EXPECT_EQ(desc.GetTotalParameters(), static_cast<uint32_t>(0));
    EXPECT_EQ(desc.GetInputSize(), static_cast<uint32_t>(0));
    EXPECT_EQ(desc.GetOutputSize(), static_cast<uint32_t>(0));
}

TEST(Neural_NetworkDescInputOutputSize)
{
    NetworkDesc desc;
    desc.layers = {{3, 8, ActivationType::ReLU}, {8, 8, ActivationType::ReLU}, {8, 2, ActivationType::Sigmoid}};
    EXPECT_EQ(desc.GetInputSize(), static_cast<uint32_t>(3));
    EXPECT_EQ(desc.GetOutputSize(), static_cast<uint32_t>(2));
}

TEST(Neural_HandleValidity)
{
    NetworkHandle h;
    EXPECT_FALSE(h.IsValid());

    NetworkHandle h2{42};
    EXPECT_TRUE(h2.IsValid());
    EXPECT_TRUE(h != h2);
}

TEST(Neural_WeightDataSize)
{
    NetworkDesc desc;
    desc.layers = {{4, 8, ActivationType::ReLU}};
    // 4*8 + 8 = 40 params, 40 * 4 bytes = 160
    EXPECT_EQ(desc.GetWeightDataSize(), static_cast<size_t>(160));
}

// ============================================================================
// CPU inference tests
// ============================================================================

TEST(Neural_CPUInferenceIdentity)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize(); // CPU-only mode

    // Single layer: 2 inputs → 2 outputs, identity weights, no activation
    NetworkDesc desc;
    desc.name = "identity";
    desc.layers = {{2, 2, ActivationType::None}};

    auto handle = engine.CreateNetwork(desc);
    EXPECT_TRUE(handle.IsValid());

    // Identity matrix weights + zero biases
    // Weight layout: [w00, w01, w10, w11, b0, b1]
    // For identity: w00=1, w01=0, w10=0, w11=1, b0=0, b1=0
    std::vector<float> weights = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    EXPECT_TRUE(engine.UploadWeights(handle, weights));

    std::vector<float> input = {3.0f, 7.0f};
    std::vector<float> output(2, 0.0f);
    engine.EvaluateCPU(handle, input.data(), output.data(), 1);

    EXPECT_TRUE(std::abs(output[0] - 3.0f) < 1e-5f);
    EXPECT_TRUE(std::abs(output[1] - 7.0f) < 1e-5f);

    engine.DestroyNetwork(handle);
}

TEST(Neural_CPUInferenceReLU)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NetworkDesc desc;
    desc.name = "relu_test";
    desc.layers = {{2, 2, ActivationType::ReLU}};

    auto handle = engine.CreateNetwork(desc);

    // Weights that produce one positive and one negative output:
    // output[0] = 1*input[0] + 0*input[1] + 0 = 1.0 → ReLU → 1.0
    // output[1] = -1*input[0] + 0*input[1] + 0 = -1.0 → ReLU → 0.0
    std::vector<float> weights = {1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    engine.UploadWeights(handle, weights);

    std::vector<float> input = {1.0f, 5.0f};
    std::vector<float> output(2, -1.0f);
    engine.EvaluateCPU(handle, input.data(), output.data(), 1);

    EXPECT_TRUE(std::abs(output[0] - 1.0f) < 1e-5f);
    EXPECT_TRUE(std::abs(output[1] - 0.0f) < 1e-5f);

    engine.DestroyNetwork(handle);
}

TEST(Neural_CPUInferenceSigmoid)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NetworkDesc desc;
    desc.name = "sigmoid_test";
    desc.layers = {{1, 1, ActivationType::Sigmoid}};

    auto handle = engine.CreateNetwork(desc);

    // w=1, b=0 → sigmoid(input)
    std::vector<float> weights = {1.0f, 0.0f};
    engine.UploadWeights(handle, weights);

    std::vector<float> input = {0.0f};
    std::vector<float> output(1, 0.0f);
    engine.EvaluateCPU(handle, input.data(), output.data(), 1);

    // sigmoid(0) = 0.5
    EXPECT_TRUE(std::abs(output[0] - 0.5f) < 1e-5f);

    engine.DestroyNetwork(handle);
}

TEST(Neural_CPUInferenceMultiLayer)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    // 2-layer MLP: 2 → 4 (ReLU) → 1 (None)
    NetworkDesc desc;
    desc.name = "multilayer";
    desc.layers = {{2, 4, ActivationType::ReLU}, {4, 1, ActivationType::None}};

    auto handle = engine.CreateNetwork(desc);

    // Layer 0: 2*4 weights + 4 biases = 12
    // Layer 1: 4*1 weights + 1 bias = 5
    // Total: 17 parameters
    EXPECT_EQ(desc.GetTotalParameters(), static_cast<uint32_t>(17));

    // All weights = 1.0, all biases = 0.0
    // Layer 0: each output = sum of inputs = 1+2 = 3, ReLU(3) = 3
    // Layer 1: output = sum of layer 0 outputs = 3*4 = 12
    std::vector<float> weights = {// Layer 0 weights (4 rows x 2 cols)
                                  1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                  // Layer 0 biases
                                  0.0f, 0.0f, 0.0f, 0.0f,
                                  // Layer 1 weights (1 row x 4 cols)
                                  1.0f, 1.0f, 1.0f, 1.0f,
                                  // Layer 1 bias
                                  0.0f};
    engine.UploadWeights(handle, weights);

    std::vector<float> input = {1.0f, 2.0f};
    std::vector<float> output(1, 0.0f);
    engine.EvaluateCPU(handle, input.data(), output.data(), 1);

    EXPECT_TRUE(std::abs(output[0] - 12.0f) < 1e-4f);

    engine.DestroyNetwork(handle);
}

TEST(Neural_CPUInferenceBatch)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NetworkDesc desc;
    desc.name = "batch_test";
    desc.layers = {{1, 1, ActivationType::None}};

    auto handle = engine.CreateNetwork(desc);

    // w=2, b=1 → output = 2*input + 1
    std::vector<float> weights = {2.0f, 1.0f};
    engine.UploadWeights(handle, weights);

    std::vector<float> input = {0.0f, 1.0f, 5.0f, -3.0f};
    std::vector<float> output(4, 0.0f);
    engine.EvaluateCPU(handle, input.data(), output.data(), 4);

    EXPECT_TRUE(std::abs(output[0] - 1.0f) < 1e-5f);    // 2*0+1
    EXPECT_TRUE(std::abs(output[1] - 3.0f) < 1e-5f);    // 2*1+1
    EXPECT_TRUE(std::abs(output[2] - 11.0f) < 1e-5f);   // 2*5+1
    EXPECT_TRUE(std::abs(output[3] - (-5.0f)) < 1e-5f); // 2*(-3)+1

    engine.DestroyNetwork(handle);
}

TEST(Neural_CPUInferenceLeakyReLU)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NetworkDesc desc;
    desc.name = "leaky_relu";
    desc.layers = {{1, 1, ActivationType::LeakyReLU}};

    auto handle = engine.CreateNetwork(desc);
    std::vector<float> weights = {1.0f, 0.0f}; // pass-through
    engine.UploadWeights(handle, weights);

    // Positive input
    std::vector<float> input1 = {5.0f};
    std::vector<float> output1(1, 0.0f);
    engine.EvaluateCPU(handle, input1.data(), output1.data(), 1);
    EXPECT_TRUE(std::abs(output1[0] - 5.0f) < 1e-5f);

    // Negative input: leaky_relu(-10) = -0.1
    std::vector<float> input2 = {-10.0f};
    std::vector<float> output2(1, 0.0f);
    engine.EvaluateCPU(handle, input2.data(), output2.data(), 1);
    EXPECT_TRUE(std::abs(output2[0] - (-0.1f)) < 1e-5f);

    engine.DestroyNetwork(handle);
}

TEST(Neural_CreateNetworkValidation)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    // Empty layers should fail
    NetworkDesc empty;
    auto h1 = engine.CreateNetwork(empty);
    EXPECT_FALSE(h1.IsValid());

    // Too many layers should fail
    NetworkDesc tooMany;
    for (uint32_t i = 0; i < 10; ++i)
    {
        tooMany.layers.push_back({4, 4, ActivationType::ReLU});
    }
    auto h2 = engine.CreateNetwork(tooMany);
    EXPECT_FALSE(h2.IsValid());
}

TEST(Neural_UploadWeightsSizeMismatch)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NetworkDesc desc;
    desc.layers = {{2, 2, ActivationType::None}};
    auto handle = engine.CreateNetwork(desc);

    // Wrong size: need 6, provide 3
    std::vector<float> badWeights = {1.0f, 2.0f, 3.0f};
    EXPECT_FALSE(engine.UploadWeights(handle, badWeights));

    engine.DestroyNetwork(handle);
}

TEST(Neural_ConsoleStatus)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    auto status = engine.Console_GetStatus();
    EXPECT_TRUE(status.find("Neural Inference Engine") != std::string::npos);
    EXPECT_TRUE(status.find("Initialized: YES") != std::string::npos);
}

// ============================================================================
// Weight I/O tests
// ============================================================================

TEST(Neural_WeightsSaveLoad)
{
    TrainedNetwork network;
    network.desc.name = "test_net";
    network.desc.layers = {{3, 8, ActivationType::ReLU}, {8, 2, ActivationType::Sigmoid}};

    uint32_t totalParams = network.desc.GetTotalParameters();
    network.weights.resize(totalParams);
    for (uint32_t i = 0; i < totalParams; ++i)
    {
        network.weights[i] = static_cast<float>(i) * 0.01f;
    }

    const std::string path = "/tmp/test_neural_weights.nnw";
    EXPECT_TRUE(SaveWeights(network, path));

    auto loaded = LoadWeights(path);
    EXPECT_EQ(loaded.desc.layers.size(), static_cast<size_t>(2));
    EXPECT_EQ(loaded.weights.size(), static_cast<size_t>(totalParams));

    for (uint32_t i = 0; i < totalParams; ++i)
    {
        EXPECT_TRUE(std::abs(loaded.weights[i] - network.weights[i]) < 1e-6f);
    }
}

TEST(Neural_WeightsLoadInvalidFile)
{
    auto loaded = LoadWeights("/tmp/nonexistent_file_12345.nnw");
    EXPECT_TRUE(loaded.desc.layers.empty());
    EXPECT_TRUE(loaded.weights.empty());
}

TEST(Neural_NetworkDescGetWeightDataSize)
{
    NetworkDesc desc;
    desc.layers = {{10, 20, ActivationType::ReLU}, {20, 5, ActivationType::None}};
    // Layer 0: 10*20 + 20 = 220
    // Layer 1: 20*5 + 5 = 105
    // Total: 325 params * 4 bytes = 1300
    EXPECT_EQ(desc.GetWeightDataSize(), static_cast<size_t>(1300));
}
