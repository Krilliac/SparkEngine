/**
 * @file NeuralFunctionApproximator.h
 * @brief Generic learned CPU function f: R^N → R^M for engine subsystems
 * @author Spark Engine Team
 * @date 2026
 *
 * Thin facade over @ref Trainer + @ref CpuNeuralInference for code that wants
 * to replace or augment a hand-written scalar function with a tiny MLP. Exposes
 * a minimal API (Configure / TrainOffline / TrainIncremental / Query /
 * Save / Load) and hides the weight buffer plumbing.
 *
 * ## When to use this vs. a hand-written function
 * A learned approximator only beats a hand-written function when:
 *  - The function is expensive (iterative solver, many branches, transcendentals)
 *  - A smooth, differentiable approximation is acceptable
 *  - A training corpus exists or can be generated
 *
 * Do NOT use for: trivial dot products, hashes, functions requiring determinism
 * across platforms (save games, networking), or anything where exact semantics
 * matter.
 *
 * ## Offline use (ship trained .nnw as an asset)
 * @code
 *   NeuralFunctionApproximator nfa;
 *   nfa.Configure(2, 1, {32, 32}); // 2 inputs, 1 output, two 32-wide hidden layers
 *   nfa.TrainOffline(trainX, trainY, numSamples, 2000); // 2000 epochs
 *   nfa.Save("Content/Neural/mycurve.nnw");
 *
 *   // …in the game:
 *   NeuralFunctionApproximator inference;
 *   inference.Load("Content/Neural/mycurve.nnw");
 *   float y = inference.QueryScalar(x0, x1);
 * @endcode
 *
 * ## In-engine online training (learn from gameplay signal)
 * @code
 *   NeuralFunctionApproximator adaptive;
 *   adaptive.Configure(4, 3, {64});
 *   // each frame / event:
 *   adaptive.TrainIncremental(features, observedOutput);
 *   // any time:
 *   adaptive.Query(features, prediction);
 * @endcode
 *
 * @see CpuNeuralTraining.h, CpuNeuralInference.h, NeuralWeights.h
 */

#pragma once

#include "CpuNeuralInference.h"
#include "CpuNeuralTraining.h"
#include "NeuralTypes.h"
#include "NeuralWeights.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Graphics::Neural
{

    /**
     * @brief Learned scalar/vector function approximator backed by a CPU MLP.
     *
     * Lifetime: cheap to construct — the first call to Configure() or Load()
     * allocates weight buffers sized to the chosen architecture. Safe to use
     * from the main thread; not thread-safe for concurrent Train+Query.
     */
    class NeuralFunctionApproximator
    {
      public:
        NeuralFunctionApproximator();
        ~NeuralFunctionApproximator();

        NeuralFunctionApproximator(const NeuralFunctionApproximator&) = delete;
        NeuralFunctionApproximator& operator=(const NeuralFunctionApproximator&) = delete;
        NeuralFunctionApproximator(NeuralFunctionApproximator&&) noexcept;
        NeuralFunctionApproximator& operator=(NeuralFunctionApproximator&&) noexcept;

        /**
         * @brief Build an architecture: @p inputSize → [hiddenSizes...] → @p outputSize.
         *
         * Hidden layers use ReLU. The output layer is linear (ActivationType::None).
         * For bounded outputs in [0, 1], call @ref SetOutputActivation() afterwards.
         *
         * @param inputSize   Dimensionality of feature vector.
         * @param outputSize  Dimensionality of output vector.
         * @param hiddenSizes Widths of hidden layers (may be empty for a linear regressor).
         * @param seed        RNG seed for Xavier init.
         */
        void Configure(uint32_t inputSize, uint32_t outputSize, const std::vector<uint32_t>& hiddenSizes,
                       uint32_t seed = 42u);

        /**
         * @brief Override the output-layer activation (e.g. Sigmoid for [0,1] targets).
         *
         * Must be called after Configure() and before any training calls.
         */
        void SetOutputActivation(ActivationType activation);

        /**
         * @brief Override the Adam optimizer config used by training calls.
         *
         * Defaults are safe for most small MLPs (lr=1e-3, betas=(0.9, 0.999)).
         */
        void SetAdamConfig(const AdamConfig& cfg) { m_adamConfig = cfg; }

        /** @brief Current Adam config (read-only access for diagnostics). */
        const AdamConfig& GetAdamConfig() const { return m_adamConfig; }

        /** @brief Check if the approximator has a valid architecture. */
        bool IsConfigured() const { return !m_desc.layers.empty(); }

        /** @brief Input dimensionality (0 if not configured). */
        uint32_t GetInputSize() const { return m_desc.GetInputSize(); }

        /** @brief Output dimensionality (0 if not configured). */
        uint32_t GetOutputSize() const { return m_desc.GetOutputSize(); }

        /**
         * @brief Train offline on a fixed dataset for @p epochs passes.
         *
         * Performs per-sample Adam updates (online SGD semantics). If you need
         * mini-batch, call @ref AccumulateBatch + @ref StepBatch directly on
         * @ref GetTrainer().
         *
         * @param inputs     Flat input buffer (numSamples * inputSize).
         * @param targets    Flat target buffer (numSamples * outputSize).
         * @param numSamples Number of training samples.
         * @param epochs     Number of full passes over the dataset.
         * @param loss       Loss function (default MSE).
         * @return           Final-epoch mean loss.
         */
        float TrainOffline(const float* inputs, const float* targets, uint32_t numSamples, uint32_t epochs,
                           LossType loss = LossType::MSE);

        /**
         * @brief Single-sample online Adam update (accumulates gradient + steps).
         *
         * Intended for in-engine adaptive use: call once per observed
         * (features, target) event. Adam moments persist across calls.
         *
         * @return Per-sample loss.
         */
        float TrainIncremental(const float* input, const float* target, LossType loss = LossType::MSE);

        /**
         * @brief Forward pass: evaluate the network at one input.
         * @param input  Feature vector (size == GetInputSize()).
         * @param output Output buffer (size == GetOutputSize()).
         */
        void Query(const float* input, float* output) const;

        /**
         * @brief Convenience: evaluate a 2-input, 1-output scalar function.
         *
         * Throws nothing — returns 0 if not configured with those shapes.
         */
        float QueryScalar(float x0, float x1) const;

        /** @brief Convenience: evaluate a 1-input, 1-output scalar function. */
        float QueryScalar(float x) const;

        /**
         * @brief Save architecture + weights (+ optimizer state) to @p path.
         * @param includeOptimizerState If true, the .nnw trailer carries Adam
         *        moments so training can resume later. Off by default (smaller file).
         * @return true on success.
         */
        bool Save(const std::string& path, bool includeOptimizerState = false) const;

        /**
         * @brief Load weights (and optional optimizer state) from @p path.
         *
         * Replaces any previously-configured architecture. Optimizer state in
         * the file, if present, is restored into the trainer; otherwise Adam
         * state is reset.
         *
         * @return true on success.
         */
        bool Load(const std::string& path);

        /** @brief Raw trainer access for advanced use (mini-batch, diagnostics). */
        Trainer& GetTrainer() { return *m_trainer; }
        const Trainer& GetTrainer() const { return *m_trainer; }

        /** @brief Raw weight buffer (size == totalParameters). */
        const std::vector<float>& GetWeights() const { return m_weights; }

      private:
        NetworkDesc m_desc;
        std::vector<float> m_weights;
        std::unique_ptr<Trainer> m_trainer;
        AdamConfig m_adamConfig;

        // Cached aligned-weight layout for inference. Lazily rebuilt when
        // weights change (every TrainIncremental call marks it dirty); kept
        // so Query() stays fast without repacking on each call.
        mutable AlignedWeightLayout m_cachedLayout;
        mutable bool m_layoutDirty = true;

        void MarkWeightsChanged() { m_layoutDirty = true; }
        void RebuildLayoutIfNeeded() const;
    };

} // namespace Spark::Graphics::Neural
