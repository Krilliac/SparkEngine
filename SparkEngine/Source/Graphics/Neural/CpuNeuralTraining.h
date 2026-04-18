/**
 * @file CpuNeuralTraining.h
 * @brief CPU neural-network training: backprop + optimizers + losses
 * @author Spark Engine Team
 * @date 2026
 *
 * Shared training substrate for all in-engine neural systems. Provides a
 * symmetric backward pass over the same flat weight layout used by
 * NeuralInferenceEngine, plus Adam/SGD optimizers and MSE/L1/Huber loss
 * functions. Designed to be used either as a one-shot offline trainer
 * (NeuralTextureCompressor) or an incremental online trainer
 * (NeuralRadianceCache, NeuralFunctionApproximator).
 *
 * ## Design
 * - Weights stored in the same flat layout as @ref NetworkDesc (weights
 *   row-major per layer, followed by biases).
 * - Backward pass recomputes pre-activations during the forward pass and
 *   caches per-layer outputs + inputs in thread-local scratch; no heap
 *   allocations inside the training loop for the steady state.
 * - Optimizer state (Adam's m/v moment buffers) lives alongside the
 *   weight buffer, same size as totalParameters.
 *
 * ## Usage — one-shot offline
 * @code
 *   Trainer t;
 *   t.Initialize(desc);                  // allocates gradients + optimizer state
 *   t.InitializeWeightsXavier(weights);  // in-place Xavier init
 *   AdamConfig adam{}; adam.learningRate = 1e-3f;
 *
 *   for (uint32_t epoch = 0; epoch < 1000; ++epoch)
 *   {
 *       float loss = t.TrainEpoch(weights, desc, inputs, targets,
 *                                  numSamples, LossType::MSE, adam);
 *   }
 * @endcode
 *
 * ## Usage — incremental online
 * @code
 *   t.TrainSample(weights, desc, input, target, LossType::MSE, adam);
 * @endcode
 *
 * @see NeuralTypes.h, CpuNeuralInference.h
 */

#pragma once

#include "NeuralTypes.h"

#include <cstdint>
#include <vector>

namespace Spark::Graphics::Neural
{

    // =========================================================================
    // Loss functions
    // =========================================================================

    enum class LossType : uint8_t
    {
        MSE,  ///< Mean-squared error:  0.5 * (y - t)^2, grad: (y - t)
        L1,   ///< Absolute error:       |y - t|,          grad: sign(y - t)
        Huber ///< Smooth-L1 (delta=1):  quadratic < 1, linear beyond
    };

    // =========================================================================
    // Optimizer configurations
    // =========================================================================

    /** @brief Plain SGD with optional learning-rate decay. */
    struct SGDConfig
    {
        float learningRate = 1e-2f;
        float momentum = 0.0f;    ///< 0 = no momentum, 0.9 = Nesterov-style
        float weightDecay = 0.0f; ///< L2 regularization coefficient
    };

    /**
     * @brief Adam optimizer configuration (Kingma & Ba 2014).
     *
     * Defaults match the reference paper and are a safe starting point for
     * small MLPs. Bias correction is applied via the external step counter.
     */
    struct AdamConfig
    {
        float learningRate = 1e-3f;
        float beta1 = 0.9f;
        float beta2 = 0.999f;
        float epsilon = 1e-8f;
        float weightDecay = 0.0f; ///< Decoupled L2 (AdamW when > 0)
    };

    // =========================================================================
    // Trainer
    // =========================================================================

    /**
     * @brief Stateful trainer for a single network architecture.
     *
     * Not thread-safe — create one Trainer per thread if you need parallel
     * training of independent networks. The EvaluateBatch path of
     * CpuNeuralInference is separately thread-safe for pure inference.
     */
    class Trainer
    {
      public:
        Trainer() = default;

        /**
         * @brief Allocate gradient buffers + optimizer state for @p desc.
         *
         * Safe to call multiple times with different descriptors (reallocates).
         * @param desc  Network architecture.
         */
        void Initialize(const NetworkDesc& desc);

        /** @brief Release gradient + optimizer buffers. */
        void Reset();

        /**
         * @brief Fill @p weights with Xavier/Glorot-normal init, biases zero.
         *
         * @param desc      Architecture
         * @param weights   Output buffer (resized to desc.GetTotalParameters())
         * @param seed      RNG seed (deterministic)
         */
        static void InitializeWeightsXavier(const NetworkDesc& desc, std::vector<float>& weights, uint32_t seed = 42u);

        /**
         * @brief Run forward pass and accumulate gradients for one sample.
         *
         * Does not update weights. Use this when you need to batch gradients
         * across multiple samples manually (e.g. NRC online update).
         *
         * @param weights   Current weights (read-only within this call).
         * @param desc      Architecture (must match Initialize()).
         * @param input     Input vector (size == desc.GetInputSize()).
         * @param target    Target vector (size == desc.GetOutputSize()).
         * @param loss      Loss function.
         * @param[out] outInputGradient  If non-null, receives the gradient
         *             of the loss w.r.t. the inputs (size == inputSize).
         *             Useful for hash-grid/feature backprop.
         * @return Scalar loss for this sample.
         */
        float AccumulateGradient(const float* weights, const NetworkDesc& desc, const float* input, const float* target,
                                 LossType loss, float* outInputGradient = nullptr);

        /** @brief Zero the accumulated gradient buffer. */
        void ZeroGradients();

        /**
         * @brief Apply one SGD step using the currently-accumulated gradient.
         * @param weights       In/out weight buffer.
         * @param cfg           Optimizer config.
         * @param gradientScale Multiplier on the gradient (e.g. 1/batchSize
         *                      after accumulating a batch). Default 1.
         */
        void StepSGD(std::vector<float>& weights, const SGDConfig& cfg, float gradientScale = 1.0f);

        /**
         * @brief Apply one Adam step using the currently-accumulated gradient.
         *
         * Bias correction uses the trainer's internal step counter, which
         * starts at 1 and increments on each call.
         */
        void StepAdam(std::vector<float>& weights, const AdamConfig& cfg, float gradientScale = 1.0f);

        /**
         * @brief Full epoch over a dataset with Adam: accumulate + step per sample.
         *
         * Uses per-sample updates (online SGD) with gradientScale = 1. For true
         * mini-batch training, call AccumulateGradient() over the batch, then
         * StepAdam(..., 1.0f / batchSize).
         *
         * @return Mean loss over the epoch.
         */
        float TrainEpoch(std::vector<float>& weights, const NetworkDesc& desc, const float* inputs,
                         const float* targets, uint32_t numSamples, LossType loss, const AdamConfig& cfg);

        /** @brief Single-sample online Adam update. */
        float TrainSample(std::vector<float>& weights, const NetworkDesc& desc, const float* input, const float* target,
                          LossType loss, const AdamConfig& cfg);

        /** @brief Access to raw gradient buffer (for diagnostics / custom updates). */
        const std::vector<float>& GetGradients() const { return m_gradients; }

        /** @brief Reset Adam step counter + moment buffers (e.g. after LR change). */
        void ResetOptimizerState();

        /**
         * @brief Numerical gradient check for a single sample (very slow — tests only).
         *
         * Returns the max relative error between analytical and finite-difference
         * gradients across all parameters. Use to validate Trainer correctness.
         */
        static float GradientCheck(const NetworkDesc& desc, const std::vector<float>& weights, const float* input,
                                   const float* target, LossType loss, float epsilon = 1e-3f);

      private:
        std::vector<float> m_gradients; ///< Size = desc.GetTotalParameters()
        std::vector<float> m_adamM;     ///< First-moment buffer
        std::vector<float> m_adamV;     ///< Second-moment buffer
        std::vector<float> m_momentum;  ///< SGD momentum buffer
        uint32_t m_adamStep = 0;
        uint32_t m_totalParams = 0;

        // Cached per-layer offsets into the flat weight buffer (computed in Initialize).
        std::vector<uint32_t> m_layerWeightOffsets;
    };

    // =========================================================================
    // Free-standing helpers
    // =========================================================================

    /** @brief Evaluate a loss + its output-gradient for one (predicted, target) pair. */
    float ComputeLoss(LossType loss, const float* predicted, const float* target, uint32_t size, float* outGradient);

} // namespace Spark::Graphics::Neural
