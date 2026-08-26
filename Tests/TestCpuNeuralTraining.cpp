/**
 * @file TestCpuNeuralTraining.cpp
 * @brief Tests for the CPU neural-network training substrate.
 *
 * Covers:
 *  - Analytical backprop correctness (numerical gradient check)
 *  - Loss functions (MSE / L1 / Huber)
 *  - Optimizers (SGD / SGD+momentum / Adam)
 *  - End-to-end training convergence (XOR, sin(x), 2D function)
 *  - NeuralFunctionApproximator save/load + query
 *  - .nnw v2 optimizer-state round-trip
 *  - NeuralRadianceCache loss decreases under the new backprop path
 */

#include "TestFramework.h"

#include "Graphics/Neural/CpuNeuralInference.h"
#include "Graphics/Neural/CpuNeuralTraining.h"
#include "Graphics/Neural/NeuralFunctionApproximator.h"
#include "Graphics/Neural/NeuralRadianceCache.h"
#include "Graphics/Neural/NeuralTypes.h"
#include "Graphics/Neural/NeuralWeights.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>
#include <vector>

using namespace Spark::Graphics::Neural;

namespace
{
    std::string NeuralTestPath(const char* fileName)
    {
        static std::atomic_uint64_t sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return (std::filesystem::temp_directory_path() /
                (std::string("spark_neural_") + fileName + "_" + std::to_string(tick) + "_" +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed))))
            .string();
    }
} // namespace

// ============================================================================
// Loss functions
// ============================================================================

TEST(Training_MSELossAndGradient)
{
    float predicted[3] = {1.0f, 2.0f, 3.0f};
    float target[3] = {0.5f, 2.5f, 2.0f};
    float grad[3] = {0, 0, 0};

    float loss = ComputeLoss(LossType::MSE, predicted, target, 3, grad);
    // 0.5 * (0.25 + 0.25 + 1.0) = 0.75
    EXPECT_NEAR(loss, 0.75f, 1e-5f);
    EXPECT_NEAR(grad[0], 0.5f, 1e-5f);
    EXPECT_NEAR(grad[1], -0.5f, 1e-5f);
    EXPECT_NEAR(grad[2], 1.0f, 1e-5f);
}

TEST(Training_L1LossAndGradient)
{
    float predicted[2] = {3.0f, -2.0f};
    float target[2] = {1.0f, 1.0f};
    float grad[2] = {0, 0};
    float loss = ComputeLoss(LossType::L1, predicted, target, 2, grad);
    EXPECT_NEAR(loss, 5.0f, 1e-5f); // |2| + |-3| = 5
    EXPECT_NEAR(grad[0], 1.0f, 1e-5f);
    EXPECT_NEAR(grad[1], -1.0f, 1e-5f);
}

TEST(Training_HuberLossSwitchesAtDelta)
{
    float p[2] = {0.5f, 3.0f};
    float t[2] = {0.0f, 0.0f};
    float grad[2] = {0, 0};
    float loss = ComputeLoss(LossType::Huber, p, t, 2, grad);
    // element 0: |0.5| <= 1 → 0.5*0.25 = 0.125 (quadratic)
    // element 1: |3|  > 1  → 1*(3 - 0.5) = 2.5 (linear)
    EXPECT_NEAR(loss, 2.625f, 1e-5f);
    EXPECT_NEAR(grad[0], 0.5f, 1e-5f);
    EXPECT_NEAR(grad[1], 1.0f, 1e-5f);
}

// ============================================================================
// Analytical-vs-numerical gradient check
// ============================================================================

TEST(Training_GradientCheckMSE)
{
    NetworkDesc desc;
    desc.layers = {{3, 4, ActivationType::ReLU}, {4, 2, ActivationType::Sigmoid}};

    std::vector<float> weights;
    Trainer::InitializeWeightsXavier(desc, weights, 7u);

    float input[3] = {0.3f, -0.7f, 1.2f};
    float target[2] = {0.6f, 0.2f};

    float relErr = Trainer::GradientCheck(desc, weights, input, target, LossType::MSE, 1e-3f);
    // Analytical backprop must agree with central-difference to ~1e-3.
    EXPECT_LT(relErr, 1e-2f);
}

TEST(Training_GradientCheckHuber)
{
    NetworkDesc desc;
    desc.layers = {{2, 8, ActivationType::Tanh}, {8, 3, ActivationType::None}};

    std::vector<float> weights;
    Trainer::InitializeWeightsXavier(desc, weights, 123u);

    float input[2] = {0.5f, -1.4f};
    float target[3] = {0.2f, 1.5f, -0.3f};

    // Use a 1e-2 perturbation for the central difference rather than 1e-3.
    // The relative error here is dominated by floating-point cancellation when
    // subtracting two nearly-equal loss values, not by truncation, so a larger
    // step is the finite-difference sweet spot: relErr drops from ~0.004 (eps
    // 1e-3, only ~2x margin and FP-mode-sensitive — it tipped over the 1e-2
    // bound under CI's MSVC Release toolset) to ~0.0004 (eps 1e-2, ~24x margin),
    // making the check stable across compilers. (Training_GradientCheckMSE keeps
    // 1e-3 since its smoother gradients pass comfortably at that step.)
    float relErr = Trainer::GradientCheck(desc, weights, input, target, LossType::Huber, 1e-2f);
    EXPECT_LT(relErr, 1e-2f);
}

// ============================================================================
// Optimizer convergence — XOR
// ============================================================================

TEST(Training_AdamConvergesOnXOR)
{
    NetworkDesc desc;
    desc.layers = {{2, 8, ActivationType::Tanh}, {8, 1, ActivationType::Sigmoid}};

    std::vector<float> weights;
    Trainer::InitializeWeightsXavier(desc, weights, 2026u);

    const float inputs[4 * 2] = {0, 0, 0, 1, 1, 0, 1, 1};
    const float targets[4] = {0, 1, 1, 0};

    Trainer trainer;
    trainer.Initialize(desc);
    AdamConfig cfg;
    cfg.learningRate = 5e-2f;

    float lastLoss = 0.0f;
    for (int epoch = 0; epoch < 2000; ++epoch)
    {
        lastLoss = trainer.TrainEpoch(weights, desc, inputs, targets, 4, LossType::MSE, cfg);
    }
    // XOR is easy — a 2-8-1 Tanh+Sigmoid net reliably drives MSE well below 0.02
    // with Adam. Use 0.05 as a very generous margin for deterministic seeds.
    EXPECT_LT(lastLoss, 0.05f);
}

TEST(Training_SGDMomentumConvergesOnXOR)
{
    NetworkDesc desc;
    desc.layers = {{2, 8, ActivationType::Tanh}, {8, 1, ActivationType::Sigmoid}};

    std::vector<float> weights;
    Trainer::InitializeWeightsXavier(desc, weights, 42u);

    const float inputs[4 * 2] = {0, 0, 0, 1, 1, 0, 1, 1};
    const float targets[4] = {0, 1, 1, 0};

    Trainer trainer;
    trainer.Initialize(desc);
    SGDConfig cfg;
    cfg.learningRate = 0.2f;
    cfg.momentum = 0.9f;

    float lastLoss = 0.0f;
    for (int epoch = 0; epoch < 4000; ++epoch)
    {
        float epochLoss = 0.0f;
        for (int s = 0; s < 4; ++s)
        {
            trainer.ZeroGradients();
            epochLoss += trainer.AccumulateGradient(weights.data(), desc, inputs + s * 2, targets + s, LossType::MSE);
            trainer.StepSGD(weights, cfg, 1.0f);
        }
        lastLoss = epochLoss / 4.0f;
    }
    EXPECT_LT(lastLoss, 0.1f);
}

// ============================================================================
// NeuralFunctionApproximator — sine fit
// ============================================================================

TEST(NFA_LearnsSineFunction)
{
    NeuralFunctionApproximator nfa;
    nfa.Configure(1, 1, {32, 32}, 99u);
    AdamConfig cfg;
    cfg.learningRate = 5e-3f;
    nfa.SetAdamConfig(cfg);

    // Generate 128 samples of sin(x) on [-pi, pi].
    constexpr int N = 128;
    std::vector<float> xs(N);
    std::vector<float> ys(N);
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist(-3.14159f, 3.14159f);
    for (int i = 0; i < N; ++i)
    {
        xs[i] = dist(rng);
        ys[i] = std::sin(xs[i]);
    }

    float finalLoss = nfa.TrainOffline(xs.data(), ys.data(), N, 800, LossType::MSE);
    EXPECT_LT(finalLoss, 0.05f);

    // Sample at a few fresh points and check prediction is within 0.25 of truth.
    for (float probe : {-2.0f, -0.5f, 0.3f, 1.1f, 2.5f})
    {
        float y = nfa.QueryScalar(probe);
        float truth = std::sin(probe);
        EXPECT_NEAR(y, truth, 0.25f);
    }
}

TEST(NFA_IncrementalTrainingLowersLoss)
{
    NeuralFunctionApproximator nfa;
    nfa.Configure(2, 1, {16}, 31u);

    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    auto measureLoss = [&](int samples)
    {
        float total = 0.0f;
        for (int i = 0; i < samples; ++i)
        {
            float x0 = dist(rng);
            float x1 = dist(rng);
            float t = x0 * x1; // target: product (smooth, learnable)
            float pred = nfa.QueryScalar(x0, x1);
            float d = pred - t;
            total += d * d;
        }
        return total / static_cast<float>(samples);
    };

    float preLoss = measureLoss(200);

    for (int i = 0; i < 4000; ++i)
    {
        float x0 = dist(rng);
        float x1 = dist(rng);
        float in[2] = {x0, x1};
        float t = x0 * x1;
        nfa.TrainIncremental(in, &t, LossType::MSE);
    }

    float postLoss = measureLoss(200);
    EXPECT_LT(postLoss, preLoss);
    EXPECT_LT(postLoss, 0.05f);
}

// ============================================================================
// NeuralFunctionApproximator — save / load round-trip
// ============================================================================

TEST(NFA_SaveLoadProducesSamePredictions)
{
    NeuralFunctionApproximator nfa;
    nfa.Configure(2, 1, {16, 8}, 11u);

    // A couple of training steps so the weights differ from init.
    for (int i = 0; i < 50; ++i)
    {
        float in[2] = {static_cast<float>(i) * 0.1f, static_cast<float>(i) * -0.05f};
        float t = in[0] + in[1];
        nfa.TrainIncremental(in, &t, LossType::MSE);
    }

    const std::string path = NeuralTestPath("nfa_roundtrip.nnw");
    ASSERT_TRUE(nfa.Save(path, false));

    NeuralFunctionApproximator loaded;
    ASSERT_TRUE(loaded.Load(path));

    EXPECT_EQ(loaded.GetInputSize(), 2u);
    EXPECT_EQ(loaded.GetOutputSize(), 1u);

    for (float probeX : {-1.0f, -0.2f, 0.3f, 0.8f})
    {
        for (float probeY : {-0.7f, 0.1f, 0.9f})
        {
            float a = nfa.QueryScalar(probeX, probeY);
            float b = loaded.QueryScalar(probeX, probeY);
            EXPECT_NEAR(a, b, 1e-5f);
        }
    }

    std::remove(path.c_str());
}

// ============================================================================
// .nnw v2 optimizer-state round-trip
// ============================================================================

TEST(NNW_V2OptimizerStateRoundTrip)
{
    TrainedNetwork net;
    net.desc.layers = {{2, 4, ActivationType::ReLU}, {4, 1, ActivationType::None}};
    const uint32_t total = net.desc.GetTotalParameters();
    net.weights.assign(total, 0.3f);

    net.optimizer.kind = OptimizerKind::Adam;
    net.optimizer.stepCount = 42;
    net.optimizer.firstMoment.assign(total, 0.01f);
    net.optimizer.secondMoment.assign(total, 0.0005f);

    const std::string path = NeuralTestPath("nnw_v2_optstate.nnw");
    ASSERT_TRUE(SaveWeights(net, path));

    TrainedNetwork loaded = LoadWeights(path);
    ASSERT_EQ(loaded.desc.layers.size(), 2u);
    ASSERT_EQ(loaded.weights.size(), total);
    EXPECT_TRUE(loaded.optimizer.kind == OptimizerKind::Adam);
    EXPECT_EQ(loaded.optimizer.stepCount, 42u);
    ASSERT_EQ(loaded.optimizer.firstMoment.size(), total);
    ASSERT_EQ(loaded.optimizer.secondMoment.size(), total);
    for (uint32_t i = 0; i < total; ++i)
    {
        EXPECT_NEAR(loaded.optimizer.firstMoment[i], 0.01f, 1e-6f);
        EXPECT_NEAR(loaded.optimizer.secondMoment[i], 0.0005f, 1e-6f);
    }

    std::remove(path.c_str());
}

TEST(NNW_V2LoadsV1FileAsNoOptimizer)
{
    // Hand-write a v1 file (4-uint32 header, no flags, no trailer) and verify
    // the v2 loader still accepts it with optimizer.kind == None.
    const std::string path = NeuralTestPath("nnw_v1_legacy.nnw");
    FILE* f = std::fopen(path.c_str(), "wb");
    ASSERT_TRUE(f != nullptr);

    // 1 layer, 1→1, totalParameters = 1*1 + 1 = 2.
    const uint32_t header[4] = {0x574E4E53u, 1u, 1u, 2u};
    std::fwrite(header, sizeof(uint32_t), 4, f);
    const uint32_t layer[3] = {1u, 1u, static_cast<uint32_t>(ActivationType::None)};
    std::fwrite(layer, sizeof(uint32_t), 3, f);
    const float weights[2] = {0.5f, 0.1f}; // weight, bias
    std::fwrite(weights, sizeof(float), 2, f);
    std::fclose(f);

    TrainedNetwork loaded = LoadWeights(path);
    EXPECT_EQ(loaded.desc.layers.size(), 1u);
    ASSERT_EQ(loaded.weights.size(), 2u);
    EXPECT_TRUE(loaded.optimizer.kind == OptimizerKind::None);
    EXPECT_NEAR(loaded.weights[0], 0.5f, 1e-6f);
    EXPECT_NEAR(loaded.weights[1], 0.1f, 1e-6f);

    std::remove(path.c_str());
}

// ============================================================================
// NeuralRadianceCache — loss decreases under new backprop
// ============================================================================

TEST(NRC_RealBackpropDecreasesLoss)
{
    NeuralRadianceCache cache;
    RadianceCacheConfig cfg;
    cfg.hashTableSize = 1024;
    cfg.mlpHiddenSize = 32;
    cfg.mlpHiddenLayers = 2;
    cfg.learningRate = 5e-3f;
    cache.Initialize(cfg);

    // Fabricate a small batch of samples around a fixed point with a consistent
    // target radiance. Training should drive the predicted radiance toward
    // the target over many batches.
    std::vector<RadianceSample> samples(32);
    for (uint32_t i = 0; i < samples.size(); ++i)
    {
        samples[i].position[0] = static_cast<float>(i % 4);
        samples[i].position[1] = static_cast<float>((i / 4) % 4);
        samples[i].position[2] = 0.0f;
        samples[i].direction[0] = 0.0f;
        samples[i].direction[1] = 0.0f;
        samples[i].direction[2] = 1.0f;
        samples[i].radiance[0] = 0.7f;
        samples[i].radiance[1] = 0.2f;
        samples[i].radiance[2] = 0.5f;
    }

    cache.Update(samples.data(), static_cast<uint32_t>(samples.size()), 0.016f);
    float initialLoss = cache.GetStats().averageTrainingLoss;

    for (int iter = 0; iter < 20; ++iter)
    {
        cache.Update(samples.data(), static_cast<uint32_t>(samples.size()), 0.016f);
    }
    float finalLoss = cache.GetStats().averageTrainingLoss;

    // New backprop path must monotonically (on average) reduce loss. Allow
    // slack — the outer loop is stochastic but a 20× iteration budget gives
    // plenty of room to beat the initial measurement.
    EXPECT_LT(finalLoss, initialLoss);
    cache.Shutdown();
}
