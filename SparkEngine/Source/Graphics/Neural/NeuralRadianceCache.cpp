/**
 * @file NeuralRadianceCache.cpp
 * @brief Multi-resolution hash grid + MLP radiance cache implementation
 */

#include "NeuralRadianceCache.h"
#include "NeuralInference.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace Spark::Graphics::Neural
{

    bool NeuralRadianceCache::Initialize(const RadianceCacheConfig& config)
    {
        if (m_initialized)
        {
            return true;
        }

        m_config = config;
        m_totalFeatureSize = kHashGridLevels * kFeaturesPerEntry;

        // Allocate hash grid storage: kHashGridLevels levels, each with tableSize * features
        uint32_t entriesPerLevel = config.hashTableSize * kFeaturesPerEntry;
        m_hashGrid.resize(kHashGridLevels);
        for (uint32_t level = 0; level < kHashGridLevels; ++level)
        {
            m_hashGrid[level].resize(entriesPerLevel, 0.0f);
        }

        // Build MLP: features + 3 (direction) → hidden → hidden → 3 (RGB)
        uint32_t mlpInputSize = m_totalFeatureSize + 3; // hash features + direction

        m_mlpDesc.name = "radiance_cache_mlp";
        m_mlpDesc.layers.push_back({mlpInputSize, config.mlpHiddenSize, ActivationType::ReLU});
        for (uint32_t i = 1; i < config.mlpHiddenLayers; ++i)
        {
            m_mlpDesc.layers.push_back({config.mlpHiddenSize, config.mlpHiddenSize, ActivationType::ReLU});
        }
        m_mlpDesc.layers.push_back({config.mlpHiddenSize, 3, ActivationType::Sigmoid});

        // Initialize MLP weights (Xavier init)
        uint32_t totalParams = m_mlpDesc.GetTotalParameters();
        m_mlpWeights.resize(totalParams);
        {
            std::mt19937 rng(12345);
            uint32_t offset = 0;
            for (const auto& layer : m_mlpDesc.layers)
            {
                float stddev = std::sqrt(2.0f / static_cast<float>(layer.inputSize + layer.outputSize));
                std::normal_distribution<float> dist(0.0f, stddev);
                uint32_t numWeights = layer.inputSize * layer.outputSize;
                for (uint32_t i = 0; i < numWeights; ++i)
                {
                    m_mlpWeights[offset++] = dist(rng);
                }
                for (uint32_t i = 0; i < layer.outputSize; ++i)
                {
                    m_mlpWeights[offset++] = 0.0f;
                }
            }
        }

        // Register MLP with inference engine
        auto& engine = NeuralInferenceEngine::GetInstance();
        m_mlpHandle = engine.CreateNetwork(m_mlpDesc);
        if (m_mlpHandle.IsValid())
        {
            engine.UploadWeights(m_mlpHandle, m_mlpWeights);
        }

        m_stats.memoryUsageBytes =
            kHashGridLevels * config.hashTableSize * kFeaturesPerEntry * sizeof(float) + totalParams * sizeof(float);

        m_initialized = true;
        return true;
    }

    void NeuralRadianceCache::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        if (m_mlpHandle.IsValid())
        {
            auto& engine = NeuralInferenceEngine::GetInstance();
            engine.DestroyNetwork(m_mlpHandle);
            m_mlpHandle = NetworkHandle{};
        }

        m_hashGrid.clear();
        m_mlpWeights.clear();
        m_initialized = false;
    }

    float NeuralRadianceCache::GetLevelResolution(uint32_t level) const
    {
        // Geometric progression from minResolution to maxResolution
        float t = static_cast<float>(level) / static_cast<float>(kHashGridLevels - 1);
        return m_config.minResolution * std::pow(m_config.maxResolution / m_config.minResolution, t);
    }

    uint32_t NeuralRadianceCache::HashPosition(const float* position, uint32_t level) const
    {
        float resolution = GetLevelResolution(level);

        // Quantize position to grid cell
        int32_t ix = static_cast<int32_t>(std::floor(position[0] / resolution));
        int32_t iy = static_cast<int32_t>(std::floor(position[1] / resolution));
        int32_t iz = static_cast<int32_t>(std::floor(position[2] / resolution));

        // Spatial hash (from Instant NGP paper — large primes)
        uint32_t hash = static_cast<uint32_t>(ix) * 1u;
        hash ^= static_cast<uint32_t>(iy) * 2654435761u;
        hash ^= static_cast<uint32_t>(iz) * 805459861u;

        return hash % m_config.hashTableSize;
    }

    void NeuralRadianceCache::LookupFeatures(const float* position, std::vector<float>& features) const
    {
        features.resize(m_totalFeatureSize);

        for (uint32_t level = 0; level < kHashGridLevels; ++level)
        {
            uint32_t idx = HashPosition(position, level);
            uint32_t featureOffset = level * kFeaturesPerEntry;
            uint32_t gridOffset = idx * kFeaturesPerEntry;

            for (uint32_t f = 0; f < kFeaturesPerEntry; ++f)
            {
                features[featureOffset + f] = m_hashGrid[level][gridOffset + f];
            }
        }
    }

    void NeuralRadianceCache::BuildMLPInput(const float* position, const float* direction,
                                            std::vector<float>& mlpInput) const
    {
        std::vector<float> features;
        LookupFeatures(position, features);

        mlpInput.resize(m_totalFeatureSize + 3);
        std::copy(features.begin(), features.end(), mlpInput.begin());
        mlpInput[m_totalFeatureSize + 0] = direction[0];
        mlpInput[m_totalFeatureSize + 1] = direction[1];
        mlpInput[m_totalFeatureSize + 2] = direction[2];
    }

    void NeuralRadianceCache::QueryCPU(const float* position, const float* direction, float* outRadiance) const
    {
        if (!m_initialized || !m_mlpHandle.IsValid())
        {
            outRadiance[0] = outRadiance[1] = outRadiance[2] = 0.0f;
            return;
        }

        std::vector<float> mlpInput;
        BuildMLPInput(position, direction, mlpInput);

        float output[3] = {0.0f, 0.0f, 0.0f};
        auto& engine = NeuralInferenceEngine::GetInstance();
        engine.EvaluateCPU(m_mlpHandle, mlpInput.data(), output, 1);

        outRadiance[0] = output[0];
        outRadiance[1] = output[1];
        outRadiance[2] = output[2];

        m_stats.totalQueries++;
    }

    void NeuralRadianceCache::QueryBatchCPU(const float* positions, const float* directions, float* outRadiance,
                                            uint32_t batchSize) const
    {
        if (!m_initialized || !m_mlpHandle.IsValid() || batchSize == 0)
        {
            std::memset(outRadiance, 0, batchSize * 3 * sizeof(float));
            return;
        }

        uint32_t mlpInputSize = m_mlpDesc.GetInputSize();

        // Build all MLP inputs into a single contiguous buffer
        std::vector<float> batchInput(batchSize * mlpInputSize);

        for (uint32_t i = 0; i < batchSize; ++i)
        {
            // Look up hash grid features for this position
            float* dst = &batchInput[i * mlpInputSize];

            for (uint32_t level = 0; level < kHashGridLevels; ++level)
            {
                uint32_t idx = HashPosition(positions + i * 3, level);
                uint32_t featureOffset = level * kFeaturesPerEntry;
                uint32_t gridOffset = idx * kFeaturesPerEntry;

                for (uint32_t f = 0; f < kFeaturesPerEntry; ++f)
                {
                    dst[featureOffset + f] = m_hashGrid[level][gridOffset + f];
                }
            }

            // Append direction
            dst[m_totalFeatureSize + 0] = directions[i * 3 + 0];
            dst[m_totalFeatureSize + 1] = directions[i * 3 + 1];
            dst[m_totalFeatureSize + 2] = directions[i * 3 + 2];
        }

        // Single batched inference call
        auto& engine = NeuralInferenceEngine::GetInstance();
        engine.EvaluateCPU(m_mlpHandle, batchInput.data(), outRadiance, batchSize);

        m_stats.totalQueries += batchSize;
    }

    void NeuralRadianceCache::Update(const RadianceSample* samples, uint32_t sampleCount, float /*deltaTime*/)
    {
        if (!m_initialized || sampleCount == 0 || !samples)
        {
            return;
        }

        float lr = m_config.learningRate;
        float totalLoss = 0.0f;

        for (uint32_t s = 0; s < sampleCount; ++s)
        {
            const auto& sample = samples[s];

            // Forward pass: build input, evaluate MLP
            std::vector<float> mlpInput;
            BuildMLPInput(sample.position, sample.direction, mlpInput);

            float predicted[3];
            auto& engine = NeuralInferenceEngine::GetInstance();
            engine.EvaluateCPU(m_mlpHandle, mlpInput.data(), predicted, 1);

            // Compute loss (MSE per sample)
            float error[3];
            for (uint32_t c = 0; c < 3; ++c)
            {
                error[c] = predicted[c] - sample.radiance[c];
                totalLoss += error[c] * error[c];
            }

            // Update hash grid entries (gradient descent on feature entries)
            // Simplified: perturb features toward reducing error
            for (uint32_t level = 0; level < kHashGridLevels; ++level)
            {
                uint32_t idx = HashPosition(sample.position, level);
                uint32_t gridOffset = idx * kFeaturesPerEntry;

                for (uint32_t f = 0; f < kFeaturesPerEntry; ++f)
                {
                    // Approximate gradient: error magnitude * learning rate
                    float grad = (error[0] + error[1] + error[2]) / 3.0f;
                    m_hashGrid[level][gridOffset + f] -= lr * grad * 0.1f;
                }
            }

            // Simple MLP weight update via finite-difference approximation
            // (Full backprop through hash grid is complex; this is a practical approximation)
            constexpr float kEps = 0.001f;
            uint32_t totalParams = m_mlpDesc.GetTotalParameters();

            // Stochastic parameter update: only update a subset of weights per sample
            uint32_t updateStride = std::max(1u, totalParams / 64);
            for (uint32_t p = s % updateStride; p < totalParams; p += updateStride)
            {
                float origWeight = m_mlpWeights[p];

                // Forward with perturbed weight
                m_mlpWeights[p] = origWeight + kEps;
                engine.UploadWeights(m_mlpHandle, m_mlpWeights);
                float perturbedOut[3];
                engine.EvaluateCPU(m_mlpHandle, mlpInput.data(), perturbedOut, 1);

                float perturbedLoss = 0.0f;
                float origLoss = 0.0f;
                for (uint32_t c = 0; c < 3; ++c)
                {
                    float pe = perturbedOut[c] - sample.radiance[c];
                    perturbedLoss += pe * pe;
                    origLoss += error[c] * error[c];
                }

                float grad = (perturbedLoss - origLoss) / kEps;
                m_mlpWeights[p] = origWeight - lr * grad;
            }

            // Re-upload corrected weights
            engine.UploadWeights(m_mlpHandle, m_mlpWeights);
        }

        m_stats.totalSamplesProcessed += sampleCount;
        m_stats.framesUpdated++;
        m_stats.averageTrainingLoss = totalLoss / static_cast<float>(sampleCount * 3);
    }

    std::string NeuralRadianceCache::Console_GetStatus() const
    {
        std::string status = "Neural Radiance Cache:\n";
        status += "  Initialized: " + std::string(m_initialized ? "YES" : "NO") + "\n";
        status += "  Hash table size: " + std::to_string(m_config.hashTableSize) + " per level\n";
        status += "  Grid levels: " + std::to_string(kHashGridLevels) + "\n";
        status += "  MLP params: " + std::to_string(m_mlpDesc.GetTotalParameters()) + "\n";
        status += "  Memory: " + std::to_string(m_stats.memoryUsageBytes / 1024) + " KB\n";
        status += "  Samples processed: " + std::to_string(m_stats.totalSamplesProcessed) + "\n";
        status += "  Queries: " + std::to_string(m_stats.totalQueries) + "\n";
        status += "  Avg loss: " + std::to_string(m_stats.averageTrainingLoss) + "\n";
        return status;
    }

} // namespace Spark::Graphics::Neural
