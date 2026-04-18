/**
 * @file NeuralRadianceCache.cpp
 * @brief Multi-resolution hash grid + MLP radiance cache implementation
 */

#include "NeuralRadianceCache.h"
#include "CpuNeuralTraining.h"
#include "NeuralInference.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

        uint32_t totalParams = m_mlpDesc.GetTotalParameters();
        Trainer::InitializeWeightsXavier(m_mlpDesc, m_mlpWeights, 12345u);

        // Register MLP with inference engine
        auto& engine = NeuralInferenceEngine::GetInstance();
        m_mlpHandle = engine.CreateNetwork(m_mlpDesc);
        if (m_mlpHandle.IsValid())
        {
            engine.UploadWeights(m_mlpHandle, m_mlpWeights);
        }

        // Build the persistent trainer. Adam's first/second moments are kept
        // across frames so the loss curve stays smooth and the finite-diff
        // approximation is retired.
        m_trainer = std::make_unique<Trainer>();
        m_trainer->Initialize(m_mlpDesc);
        m_adamConfig.learningRate = config.learningRate;

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
        m_trainer.reset();
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
        if (!m_initialized || sampleCount == 0 || !samples || !m_trainer)
        {
            return;
        }

        // Real-backprop update: accumulate exact analytical gradients for the
        // MLP weights + biases, and exact analytical input-gradients for the
        // hash-grid features (since they enter the MLP linearly in the input
        // layer). Replaces the previous O(totalParams * samples) finite-diff
        // path — ~100× faster for the default network size, and strictly more
        // accurate than the per-weight perturbation heuristic.
        const uint32_t mlpInputSize = m_mlpDesc.GetInputSize();
        thread_local std::vector<float> mlpInput;
        thread_local std::vector<float> inputGrad;
        mlpInput.resize(mlpInputSize);
        inputGrad.resize(mlpInputSize);

        m_trainer->ZeroGradients();
        float totalLoss = 0.0f;

        for (uint32_t s = 0; s < sampleCount; ++s)
        {
            const auto& sample = samples[s];
            BuildMLPInput(sample.position, sample.direction, mlpInput);

            totalLoss += m_trainer->AccumulateGradient(m_mlpWeights.data(), m_mlpDesc, mlpInput.data(), sample.radiance,
                                                       LossType::MSE, inputGrad.data());

            // Write hash-grid gradients back. The first m_totalFeatureSize
            // entries of inputGrad correspond to concatenated features from
            // kHashGridLevels levels; the last 3 are direction gradients
            // (ignored — direction is an external input, not a learned feature).
            const float lrFeatures = m_adamConfig.learningRate;
            for (uint32_t level = 0; level < kHashGridLevels; ++level)
            {
                const uint32_t idx = HashPosition(sample.position, level);
                const uint32_t gridOffset = idx * kFeaturesPerEntry;
                const uint32_t featureOffset = level * kFeaturesPerEntry;
                for (uint32_t f = 0; f < kFeaturesPerEntry; ++f)
                {
                    m_hashGrid[level][gridOffset + f] -= lrFeatures * inputGrad[featureOffset + f];
                }
            }
        }

        // One batched Adam step on the MLP, scaled by 1/batch to keep LR
        // semantics consistent with the reference literature.
        m_trainer->StepAdam(m_mlpWeights, m_adamConfig, 1.0f / static_cast<float>(sampleCount));

        // Re-upload once per batch instead of once per weight-perturbation.
        auto& engine = NeuralInferenceEngine::GetInstance();
        engine.UploadWeights(m_mlpHandle, m_mlpWeights);

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
