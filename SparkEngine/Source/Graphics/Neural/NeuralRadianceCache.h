/**
 * @file NeuralRadianceCache.h
 * @brief Multi-resolution hash grid + MLP for cached indirect lighting
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a neural radiance cache inspired by Instant NGP / NVIDIA's
 * Neural Radiance Cache. Stores learned radiance in a compact multi-resolution
 * hash grid, decoded by a small MLP. Updated incrementally each frame from
 * path-traced or screen-space samples.
 *
 * ## Pipeline
 * ```
 * Update():   feed (position, direction, radiance) samples → train hash grid + MLP
 * Query():    (position, direction) → approximate radiance (RGB)
 * ```
 *
 * ## Usage
 * @code
 *   NeuralRadianceCache cache;
 *   cache.Initialize();
 *
 *   // Each frame: update with new samples
 *   cache.Update(samples, sampleCount, deltaTime);
 *
 *   // Query indirect lighting at a point
 *   auto radiance = cache.QueryCPU(position, direction);
 * @endcode
 *
 * @see NeuralInference.h, NeuralTypes.h
 */

#pragma once

#include "CpuNeuralTraining.h"
#include "NeuralTypes.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Graphics::Neural
{

    /** @brief Number of resolution levels in the hash grid. */
    static constexpr uint32_t kHashGridLevels = 16;

    /** @brief Features per hash grid entry. */
    static constexpr uint32_t kFeaturesPerEntry = 2;

    /** @brief Default hash table size per level (64K entries). */
    static constexpr uint32_t kDefaultHashTableSize = 65536;

    /** @brief A single radiance sample for cache training. */
    struct RadianceSample
    {
        float position[3];  ///< World-space position
        float direction[3]; ///< View direction (normalized)
        float radiance[3];  ///< RGB radiance value
    };

    /** @brief Configuration for the neural radiance cache. */
    struct RadianceCacheConfig
    {
        uint32_t hashTableSize = kDefaultHashTableSize; ///< Entries per resolution level
        uint32_t mlpHiddenSize = 64;                    ///< Neurons per hidden layer
        uint32_t mlpHiddenLayers = 2;                   ///< Number of hidden layers
        float learningRate = 0.01f;                     ///< SGD learning rate
        float minResolution = 1.0f;                     ///< Finest grid resolution (world units)
        float maxResolution = 1024.0f;                  ///< Coarsest grid resolution
        float temporalBlend = 0.9f;                     ///< EMA blend with previous frame
    };

    /** @brief Per-level hash grid statistics. */
    struct HashGridLevelStats
    {
        uint32_t occupiedEntries = 0; ///< Non-zero entries
        uint32_t collisions = 0;      ///< Hash collision count
    };

    /** @brief Overall cache statistics. */
    struct RadianceCacheStats
    {
        uint32_t totalSamplesProcessed = 0;
        uint32_t totalQueries = 0;
        uint32_t framesUpdated = 0;
        float averageTrainingLoss = 0.0f;
        size_t memoryUsageBytes = 0;
    };

    /**
     * @brief Neural radiance cache with multi-resolution hash grid.
     *
     * The hash grid provides spatial encoding: a 3D position is hashed at
     * multiple resolutions, yielding feature vectors that are concatenated
     * and fed through a small MLP to produce radiance.
     */
    class NeuralRadianceCache
    {
      public:
        NeuralRadianceCache() = default;

        /**
         * @brief Initialize the cache with the given configuration.
         * @param config Cache settings
         * @return true on success
         */
        bool Initialize(const RadianceCacheConfig& config = {});

        /** @brief Shut down and free all resources. */
        void Shutdown();

        /** @brief Check if initialized. */
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Update the cache with new radiance samples.
         *
         * Performs one round of stochastic gradient descent on the hash grid
         * entries and MLP weights using the provided samples.
         *
         * @param samples Array of radiance samples
         * @param sampleCount Number of samples
         * @param deltaTime Frame delta time (for temporal blending)
         */
        void Update(const RadianceSample* samples, uint32_t sampleCount, float deltaTime);

        /**
         * @brief Query the cache for radiance at a world position + direction (CPU).
         * @param position World-space position (3 floats)
         * @param direction View direction (3 floats, normalized)
         * @param outRadiance Output RGB radiance (3 floats)
         */
        void QueryCPU(const float* position, const float* direction, float* outRadiance) const;

        /**
         * @brief Query a batch of positions (CPU).
         * @param positions Array of positions (batchSize * 3 floats)
         * @param directions Array of directions (batchSize * 3 floats)
         * @param outRadiance Output radiance (batchSize * 3 floats)
         * @param batchSize Number of queries
         */
        void QueryBatchCPU(const float* positions, const float* directions, float* outRadiance,
                           uint32_t batchSize) const;

        /** @brief Get cache statistics. */
        RadianceCacheStats GetStats() const { return m_stats; }

        /** @brief Console status string. */
        std::string Console_GetStatus() const;

      private:
        /** @brief Compute hash for a 3D position at a given resolution level. */
        uint32_t HashPosition(const float* position, uint32_t level) const;

        /** @brief Look up features from hash grid for a position. */
        void LookupFeatures(const float* position, std::vector<float>& features) const;

        /** @brief Build the full MLP input from position + direction features. */
        void BuildMLPInput(const float* position, const float* direction, std::vector<float>& mlpInput) const;

        /** @brief Get the grid resolution at a given level. */
        float GetLevelResolution(uint32_t level) const;

        bool m_initialized = false;
        RadianceCacheConfig m_config;

        // Hash grid: [level][entry] = feature vector
        std::vector<std::vector<float>> m_hashGrid; // Flat: level * tableSize * features
        uint32_t m_totalFeatureSize = 0;            // kHashGridLevels * kFeaturesPerEntry

        // MLP weights for radiance decoding
        NetworkDesc m_mlpDesc;
        NetworkHandle m_mlpHandle;
        std::vector<float> m_mlpWeights;

        // Persistent trainer: Adam moments survive across Update() calls so the
        // cache continues to learn smoothly frame-to-frame. Held by unique_ptr
        // to keep NeuralRadianceCache move-friendly if ever needed.
        std::unique_ptr<Trainer> m_trainer;
        AdamConfig m_adamConfig;

        // Statistics
        mutable RadianceCacheStats m_stats;
    };

} // namespace Spark::Graphics::Neural
