/**
 * @file CpuNeuralInference.h
 * @brief SIMD-optimized CPU neural network inference
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides SSE2/AVX2-accelerated MLP forward pass with multi-threaded
 * batch evaluation via JobSystem. Replaces the naive scalar CPU fallback
 * in NeuralInferenceEngine with vectorized dot products and parallel
 * sample dispatch.
 *
 * ## Usage
 * @code
 *   CpuNeuralInference::Initialize(); // once at startup
 *
 *   auto layout = CpuNeuralInference::PrepareWeights(desc, weights);
 *   CpuNeuralInference::EvaluateBatch(layout, input, output, batchSize);
 * @endcode
 *
 * @see NeuralInference.h, MultiISA.h, JobSystem.h
 */

#pragma once

#include "NeuralTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics::Neural
{

    /**
     * @brief SIMD-friendly weight layout with rows padded to 8-float alignment.
     *
     * Weights are repacked from the flat CPU format so that each row of the
     * weight matrix is padded to a multiple of 8 floats (32 bytes), enabling
     * aligned AVX2 loads without tail handling in the inner loop.
     */
    struct AlignedWeightLayout
    {
        /** @brief Padded weight + bias data (32-byte aligned allocation). */
        std::vector<float> data;

        uint32_t layerCount = 0;

        /** @brief Per-layer metadata for navigating the packed buffer. */
        struct LayerInfo
        {
            uint32_t inputSize = 0;
            uint32_t outputSize = 0;
            uint32_t paddedInputSize = 0; ///< inputSize rounded up to multiple of 8
            uint32_t weightOffset = 0;    ///< Offset into data[] for this layer's weights
            uint32_t biasOffset = 0;      ///< Offset into data[] for this layer's biases
            ActivationType activation = ActivationType::None;
        };

        LayerInfo layers[kMaxNetworkLayers] = {};

        /** @brief Input size of the first layer. */
        uint32_t GetInputSize() const { return layerCount > 0 ? layers[0].inputSize : 0; }

        /** @brief Output size of the last layer. */
        uint32_t GetOutputSize() const { return layerCount > 0 ? layers[layerCount - 1].outputSize : 0; }
    };

    /**
     * @brief SIMD-accelerated CPU MLP inference with multi-threaded batching.
     *
     * All methods are static. Call Initialize() once at startup to detect
     * the best ISA level and bind the optimized layer-evaluation kernel.
     */
    class CpuNeuralInference
    {
      public:
        /**
         * @brief Detect CPU ISA and bind the fastest layer-evaluation kernel.
         *
         * Must be called once before any EvaluateBatch() calls.
         * Safe to call multiple times (idempotent).
         */
        static void Initialize();

        /** @brief Check if Initialize() has been called. */
        static bool IsInitialized();

        /**
         * @brief Repack flat weights into a padded, SIMD-friendly layout.
         *
         * Each weight-matrix row is padded to a multiple of 8 floats with
         * zero-fill. Biases are stored contiguously after each layer's weight block.
         *
         * @param desc  Network architecture descriptor
         * @param weights  Flat float32 weight array (size == desc.GetTotalParameters())
         * @return Padded layout ready for EvaluateBatch()
         */
        static AlignedWeightLayout PrepareWeights(const NetworkDesc& desc, const float* weights);

        /**
         * @brief Evaluate an MLP on a batch of input samples.
         *
         * Uses SIMD-accelerated layer evaluation and splits across
         * JobSystem workers when batchSize >= kParallelBatchThreshold.
         *
         * @param layout  Prepared weight layout from PrepareWeights()
         * @param input   Input data (batchSize * layout.GetInputSize() floats)
         * @param output  Output data (batchSize * layout.GetOutputSize() floats)
         * @param batchSize  Number of input samples
         */
        static void EvaluateBatch(const AlignedWeightLayout& layout, const float* input, float* output,
                                  uint32_t batchSize);

        /** @brief Get the name of the active ISA level ("SSE2", "AVX2", etc.). */
        static const char* GetActiveISAName();

        /** @brief Minimum batch size before engaging JobSystem parallelism. */
        static constexpr uint32_t kParallelBatchThreshold = 16;

      private:
        /**
         * @brief Evaluate a single sample through all layers of the network.
         *
         * Called per-sample (potentially from multiple threads).
         * Uses thread-local scratch buffers internally.
         */
        static void EvaluateSingle(const AlignedWeightLayout& layout, const float* input, float* output);
    };

} // namespace Spark::Graphics::Neural
