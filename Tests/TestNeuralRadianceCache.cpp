/**
 * @file TestNeuralRadianceCache.cpp
 * @brief Tests for neural radiance cache
 */

#include "TestFramework.h"
#include "Graphics/Neural/NeuralRadianceCache.h"
#include "Graphics/Neural/NeuralInference.h"

#include <cmath>
#include <vector>

using namespace Spark::Graphics::Neural;

TEST(NRC_Initialize)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralRadianceCache cache;
    RadianceCacheConfig config;
    config.hashTableSize = 1024; // Small for tests
    config.mlpHiddenSize = 16;
    config.mlpHiddenLayers = 1;

    EXPECT_TRUE(cache.Initialize(config));
    EXPECT_TRUE(cache.IsInitialized());

    cache.Shutdown();
    EXPECT_FALSE(cache.IsInitialized());
}

TEST(NRC_QueryReturnsValues)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralRadianceCache cache;
    RadianceCacheConfig config;
    config.hashTableSize = 1024;
    config.mlpHiddenSize = 16;
    config.mlpHiddenLayers = 1;
    cache.Initialize(config);

    float position[3] = {10.0f, 5.0f, 3.0f};
    float direction[3] = {0.0f, 0.0f, 1.0f};
    float radiance[3] = {-999.0f, -999.0f, -999.0f};

    cache.QueryCPU(position, direction, radiance);

    // Output should be valid floats (sigmoid output: [0, 1])
    EXPECT_TRUE(radiance[0] >= 0.0f && radiance[0] <= 1.0f);
    EXPECT_TRUE(radiance[1] >= 0.0f && radiance[1] <= 1.0f);
    EXPECT_TRUE(radiance[2] >= 0.0f && radiance[2] <= 1.0f);

    cache.Shutdown();
}

TEST(NRC_UpdateProcessesSamples)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralRadianceCache cache;
    RadianceCacheConfig config;
    config.hashTableSize = 256;
    config.mlpHiddenSize = 8;
    config.mlpHiddenLayers = 1;
    config.learningRate = 0.01f;
    cache.Initialize(config);

    // Create some training samples
    std::vector<RadianceSample> samples = {
        {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}}, // Red at origin
        {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, // Green at (1,0,0)
        {{0, 1, 0}, {0, 0, 1}, {0, 0, 1}}, // Blue at (0,1,0)
    };

    cache.Update(samples.data(), static_cast<uint32_t>(samples.size()), 0.016f);

    auto stats = cache.GetStats();
    EXPECT_EQ(stats.totalSamplesProcessed, static_cast<uint32_t>(3));
    EXPECT_EQ(stats.framesUpdated, static_cast<uint32_t>(1));

    cache.Shutdown();
}

TEST(NRC_BatchQuery)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralRadianceCache cache;
    RadianceCacheConfig config;
    config.hashTableSize = 256;
    config.mlpHiddenSize = 8;
    config.mlpHiddenLayers = 1;
    cache.Initialize(config);

    std::vector<float> positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<float> directions = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    std::vector<float> radiance(9, -1.0f);

    cache.QueryBatchCPU(positions.data(), directions.data(), radiance.data(), 3);

    // All outputs should be valid
    for (int i = 0; i < 9; ++i)
    {
        EXPECT_TRUE(radiance[i] >= 0.0f && radiance[i] <= 1.0f);
    }

    auto stats = cache.GetStats();
    EXPECT_EQ(stats.totalQueries, static_cast<uint32_t>(3));

    cache.Shutdown();
}

TEST(NRC_ConsistentQueries)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralRadianceCache cache;
    RadianceCacheConfig config;
    config.hashTableSize = 256;
    config.mlpHiddenSize = 8;
    config.mlpHiddenLayers = 1;
    cache.Initialize(config);

    float position[3] = {5.0f, 5.0f, 5.0f};
    float direction[3] = {1.0f, 0.0f, 0.0f};

    float rad1[3], rad2[3];
    cache.QueryCPU(position, direction, rad1);
    cache.QueryCPU(position, direction, rad2);

    // Same input should produce same output (deterministic)
    EXPECT_TRUE(std::abs(rad1[0] - rad2[0]) < 1e-6f);
    EXPECT_TRUE(std::abs(rad1[1] - rad2[1]) < 1e-6f);
    EXPECT_TRUE(std::abs(rad1[2] - rad2[2]) < 1e-6f);

    cache.Shutdown();
}

TEST(NRC_ConsoleStatus)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralRadianceCache cache;
    RadianceCacheConfig config;
    config.hashTableSize = 256;
    config.mlpHiddenSize = 8;
    config.mlpHiddenLayers = 1;
    cache.Initialize(config);

    auto status = cache.Console_GetStatus();
    EXPECT_TRUE(status.find("Neural Radiance Cache") != std::string::npos);
    EXPECT_TRUE(status.find("Initialized: YES") != std::string::npos);

    cache.Shutdown();
}

TEST(NRC_UninitializedQuerySafe)
{
    NeuralRadianceCache cache;
    float position[3] = {0, 0, 0};
    float direction[3] = {0, 0, 1};
    float radiance[3] = {-1, -1, -1};

    // Should not crash, should return zeros
    cache.QueryCPU(position, direction, radiance);
    EXPECT_TRUE(std::abs(radiance[0]) < 1e-6f);
    EXPECT_TRUE(std::abs(radiance[1]) < 1e-6f);
    EXPECT_TRUE(std::abs(radiance[2]) < 1e-6f);
}
