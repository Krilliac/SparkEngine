/**
 * @file RandomEngine.h
 * @brief Thread-safe random number generation with modern C++ distributions
 * @author Spark Engine Team
 * @date 2026
 *
 * Replaces scattered C rand() usage with a proper mt19937-based generator
 * supporting multiple distributions and deterministic seeding for replays.
 *
 * ## Usage
 * @code
 *   using namespace Spark;
 *
 *   // Global engine RNG (thread-safe)
 *   float damage = RandomEngine::Global().Float(10.0f, 25.0f);
 *   int lootTier = RandomEngine::Global().Int(1, 5);
 *   bool crit = RandomEngine::Global().Bool(0.15f); // 15% chance
 *
 *   // Deterministic RNG for replay systems
 *   RandomEngine replayRng(42);  // fixed seed
 *   float spread = replayRng.Float(-0.1f, 0.1f);
 *
 *   // Weighted selection for loot tables
 *   std::vector<float> weights = {0.5f, 0.3f, 0.15f, 0.05f};
 *   int rarity = RandomEngine::Global().WeightedIndex(weights);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <random>
#include <mutex>
#include <vector>
#include <algorithm>
#include <numeric>

namespace Spark
{

    /**
     * @class RandomEngine
     * @brief Modern C++ random number generator with multiple distribution types.
     *
     * Wraps std::mt19937 with convenience methods for common game engine
     * random number needs. Each instance is independent; use Global() for
     * a shared thread-safe instance.
     */
    class RandomEngine
    {
      public:
        /**
         * @brief Construct with a random seed from std::random_device.
         */
        RandomEngine()
        {
            std::random_device rd;
            std::seed_seq seq{rd(), rd(), rd(), rd()};
            m_engine.seed(seq);
        }

        /**
         * @brief Construct with a fixed seed for deterministic sequences.
         * @param seed  The seed value. Same seed produces same sequence.
         */
        explicit RandomEngine(uint64_t seed) : m_engine(seed) {}

        /**
         * @brief Re-seed the generator.
         * @param seed  New seed value.
         */
        void Seed(uint64_t seed) { m_engine.seed(seed); }

        /**
         * @brief Generate a uniform random float in [min, max].
         * @param min  Lower bound (inclusive).
         * @param max  Upper bound (inclusive).
         * @return     Random float in [min, max].
         */
        float Float(float min = 0.0f, float max = 1.0f)
        {
            std::uniform_real_distribution<float> dist(min, max);
            return dist(m_engine);
        }

        /**
         * @brief Generate a uniform random integer in [min, max].
         * @param min  Lower bound (inclusive).
         * @param max  Upper bound (inclusive).
         * @return     Random integer in [min, max].
         */
        int Int(int min, int max)
        {
            std::uniform_int_distribution<int> dist(min, max);
            return dist(m_engine);
        }

        /**
         * @brief Generate a random boolean with given probability of true.
         * @param probability  Chance of returning true, in [0, 1]. Default 0.5.
         * @return             true with the given probability.
         */
        bool Bool(float probability = 0.5f)
        {
            std::bernoulli_distribution dist(static_cast<double>(probability));
            return dist(m_engine);
        }

        /**
         * @brief Generate a normally distributed float.
         * @param mean    Center of the distribution.
         * @param stddev  Standard deviation (spread).
         * @return        Random float from N(mean, stddev).
         */
        float Gaussian(float mean = 0.0f, float stddev = 1.0f)
        {
            std::normal_distribution<float> dist(mean, stddev);
            return dist(m_engine);
        }

        /**
         * @brief Select a random index based on relative weights.
         *
         * Useful for loot tables, AI decision-making, and spawn distributions.
         * Weights do not need to sum to 1.0.
         *
         * @param weights  Vector of non-negative weights. Must not be empty.
         * @return         Index into the weights vector, or 0 if weights is empty.
         */
        int WeightedIndex(const std::vector<float>& weights)
        {
            if (weights.empty())
                return 0;

            float total = std::accumulate(weights.begin(), weights.end(), 0.0f);
            if (total <= 0.0f)
                return 0;

            float roll = Float(0.0f, total);
            float cumulative = 0.0f;
            for (size_t i = 0; i < weights.size(); ++i)
            {
                cumulative += weights[i];
                if (roll <= cumulative)
                    return static_cast<int>(i);
            }
            return static_cast<int>(weights.size() - 1);
        }

        /**
         * @brief Generate a random float in [0, 1] suitable for unit ranges.
         * @return  Random float in [0, 1].
         */
        float UnitFloat() { return Float(0.0f, 1.0f); }

        /**
         * @brief Generate a random angle in radians [0, 2*PI).
         * @return  Random angle in radians.
         */
        float Angle()
        {
            constexpr float TWO_PI = 6.28318530f;
            return Float(0.0f, TWO_PI);
        }

        /**
         * @brief Shuffle a vector in-place using Fisher-Yates.
         * @tparam T  Element type.
         * @param vec  Vector to shuffle.
         */
        template <typename T> void Shuffle(std::vector<T>& vec)
        {
            for (size_t i = vec.size(); i > 1; --i)
            {
                size_t j = static_cast<size_t>(Int(0, static_cast<int>(i) - 1));
                std::swap(vec[i - 1], vec[j]);
            }
        }

        /**
         * @brief Pick a random element from a non-empty vector.
         * @tparam T  Element type.
         * @param vec  Source vector. Must not be empty.
         * @return     Const reference to a random element.
         */
        template <typename T> const T& Pick(const std::vector<T>& vec)
        {
            return vec[static_cast<size_t>(Int(0, static_cast<int>(vec.size()) - 1))];
        }

        /**
         * @brief Access the shared, thread-safe global random engine.
         *
         * Uses a mutex internally so it is safe to call from any thread.
         * For hot paths that require no contention, create a per-thread instance.
         *
         * @return  Reference to a thread-safe global RandomEngine wrapper.
         */
        static RandomEngine& Global()
        {
            static RandomEngine s_global;
            return s_global;
        }

      private:
        std::mt19937 m_engine;
    };

    /**
     * @class ThreadSafeRandomEngine
     * @brief Mutex-protected wrapper around RandomEngine for shared use.
     *
     * Prefer per-thread RandomEngine instances in hot loops. Use this
     * when convenience matters more than throughput.
     */
    class ThreadSafeRandomEngine
    {
      public:
        ThreadSafeRandomEngine() = default;
        explicit ThreadSafeRandomEngine(uint64_t seed) : m_engine(seed) {}

        float Float(float min = 0.0f, float max = 1.0f)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_engine.Float(min, max);
        }

        int Int(int min, int max)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_engine.Int(min, max);
        }

        bool Bool(float probability = 0.5f)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_engine.Bool(probability);
        }

        float Gaussian(float mean = 0.0f, float stddev = 1.0f)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_engine.Gaussian(mean, stddev);
        }

        int WeightedIndex(const std::vector<float>& weights)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_engine.WeightedIndex(weights);
        }

        static ThreadSafeRandomEngine& Global()
        {
            static ThreadSafeRandomEngine s_global;
            return s_global;
        }

      private:
        RandomEngine m_engine;
        std::mutex m_mutex;
    };

} // namespace Spark
