/**
 * @file CpuNeuralTraining.cpp
 * @brief CPU MLP training: backprop + SGD/Adam + MSE/L1/Huber losses
 */

#include "CpuNeuralTraining.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

namespace Spark::Graphics::Neural
{

    // =========================================================================
    // Activation + derivative helpers
    // =========================================================================

    static inline float ActivationForward(float x, ActivationType act)
    {
        switch (act)
        {
        case ActivationType::ReLU:
            return x > 0.0f ? x : 0.0f;
        case ActivationType::LeakyReLU:
            return x > 0.0f ? x : 0.01f * x;
        case ActivationType::Sigmoid:
            return 1.0f / (1.0f + std::exp(-x));
        case ActivationType::Tanh:
            return std::tanh(x);
        case ActivationType::None:
            return x;
        }
        return x;
    }

    // Derivative evaluated from the post-activation output y = f(x).
    static inline float ActivationDerivFromOutput(float y, ActivationType act)
    {
        switch (act)
        {
        case ActivationType::ReLU:
            return y > 0.0f ? 1.0f : 0.0f;
        case ActivationType::LeakyReLU:
            return y > 0.0f ? 1.0f : 0.01f;
        case ActivationType::Sigmoid:
            return y * (1.0f - y);
        case ActivationType::Tanh:
            return 1.0f - y * y;
        case ActivationType::None:
            return 1.0f;
        }
        return 1.0f;
    }

    // =========================================================================
    // Loss functions
    // =========================================================================

    float ComputeLoss(LossType loss, const float* predicted, const float* target, uint32_t size, float* outGradient)
    {
        float total = 0.0f;
        switch (loss)
        {
        case LossType::MSE:
        {
            for (uint32_t i = 0; i < size; ++i)
            {
                float d = predicted[i] - target[i];
                total += 0.5f * d * d;
                if (outGradient)
                {
                    outGradient[i] = d;
                }
            }
            break;
        }
        case LossType::L1:
        {
            for (uint32_t i = 0; i < size; ++i)
            {
                float d = predicted[i] - target[i];
                total += std::fabs(d);
                if (outGradient)
                {
                    outGradient[i] = d > 0.0f ? 1.0f : (d < 0.0f ? -1.0f : 0.0f);
                }
            }
            break;
        }
        case LossType::Huber:
        {
            constexpr float kDelta = 1.0f;
            for (uint32_t i = 0; i < size; ++i)
            {
                float d = predicted[i] - target[i];
                float absd = std::fabs(d);
                if (absd <= kDelta)
                {
                    total += 0.5f * d * d;
                    if (outGradient)
                    {
                        outGradient[i] = d;
                    }
                }
                else
                {
                    total += kDelta * (absd - 0.5f * kDelta);
                    if (outGradient)
                    {
                        outGradient[i] = d > 0.0f ? kDelta : -kDelta;
                    }
                }
            }
            break;
        }
        }
        return total;
    }

    // =========================================================================
    // Trainer::Initialize / Reset
    // =========================================================================

    void Trainer::Initialize(const NetworkDesc& desc)
    {
        m_totalParams = desc.GetTotalParameters();
        m_gradients.assign(m_totalParams, 0.0f);
        m_adamM.assign(m_totalParams, 0.0f);
        m_adamV.assign(m_totalParams, 0.0f);
        m_momentum.assign(m_totalParams, 0.0f);
        m_adamStep = 0;

        m_layerWeightOffsets.clear();
        m_layerWeightOffsets.reserve(desc.layers.size());
        uint32_t offset = 0;
        for (const auto& layer : desc.layers)
        {
            m_layerWeightOffsets.push_back(offset);
            offset += layer.inputSize * layer.outputSize + layer.outputSize;
        }
    }

    void Trainer::Reset()
    {
        m_gradients.clear();
        m_adamM.clear();
        m_adamV.clear();
        m_momentum.clear();
        m_layerWeightOffsets.clear();
        m_adamStep = 0;
        m_totalParams = 0;
    }

    void Trainer::ZeroGradients()
    {
        std::fill(m_gradients.begin(), m_gradients.end(), 0.0f);
    }

    void Trainer::ResetOptimizerState()
    {
        std::fill(m_adamM.begin(), m_adamM.end(), 0.0f);
        std::fill(m_adamV.begin(), m_adamV.end(), 0.0f);
        std::fill(m_momentum.begin(), m_momentum.end(), 0.0f);
        m_adamStep = 0;
    }

    // =========================================================================
    // Weight initialization (Xavier/Glorot normal)
    // =========================================================================

    void Trainer::InitializeWeightsXavier(const NetworkDesc& desc, std::vector<float>& weights, uint32_t seed)
    {
        weights.assign(desc.GetTotalParameters(), 0.0f);
        std::mt19937 rng(seed);
        uint32_t offset = 0;
        for (const auto& layer : desc.layers)
        {
            float stddev = std::sqrt(2.0f / static_cast<float>(layer.inputSize + layer.outputSize));
            std::normal_distribution<float> dist(0.0f, stddev);
            uint32_t numWeights = layer.inputSize * layer.outputSize;
            for (uint32_t i = 0; i < numWeights; ++i)
            {
                weights[offset++] = dist(rng);
            }
            // Biases initialised to zero (already from assign above).
            offset += layer.outputSize;
        }
    }

    // =========================================================================
    // Forward + backward pass on one sample
    // =========================================================================

    float Trainer::AccumulateGradient(const float* weights, const NetworkDesc& desc, const float* input,
                                      const float* target, LossType loss, float* outInputGradient)
    {
        const uint32_t layerCount = static_cast<uint32_t>(desc.layers.size());
        if (layerCount == 0 || m_totalParams == 0)
        {
            return 0.0f;
        }

        // Per-layer outputs (post-activation) and input snapshots. Using thread_local
        // buffers keeps the steady state alloc-free; EvaluateSingle in CpuNeuralInference
        // uses the same pattern.
        thread_local std::vector<std::vector<float>> layerOutputs;
        thread_local std::vector<const float*> layerInputPtrs;
        layerOutputs.resize(layerCount);
        layerInputPtrs.resize(layerCount);

        // ---- Forward ----
        const float* layerIn = input;
        for (uint32_t l = 0; l < layerCount; ++l)
        {
            const auto& layer = desc.layers[l];
            layerOutputs[l].resize(layer.outputSize);
            layerInputPtrs[l] = layerIn;

            const uint32_t wOff = m_layerWeightOffsets[l];
            const float* w = weights + wOff;
            const float* b = w + layer.inputSize * layer.outputSize;

            for (uint32_t j = 0; j < layer.outputSize; ++j)
            {
                float sum = b[j];
                const float* row = w + j * layer.inputSize;
                for (uint32_t i = 0; i < layer.inputSize; ++i)
                {
                    sum += row[i] * layerIn[i];
                }
                layerOutputs[l][j] = ActivationForward(sum, layer.activation);
            }
            layerIn = layerOutputs[l].data();
        }

        // ---- Output-layer loss + gradient ----
        const uint32_t outSize = desc.GetOutputSize();
        thread_local std::vector<float> delta;
        delta.resize(outSize);
        const float* predicted = layerOutputs[layerCount - 1].data();
        float lossVal = ComputeLoss(loss, predicted, target, outSize, delta.data());

        // ---- Backward ----
        for (int32_t l = static_cast<int32_t>(layerCount) - 1; l >= 0; --l)
        {
            const auto& layer = desc.layers[static_cast<size_t>(l)];
            const uint32_t inSize = layer.inputSize;
            const uint32_t outSz = layer.outputSize;

            // delta already has size outSz at this point (either from loss or from previous iter).
            // Multiply by activation derivative to finish the pre-activation gradient.
            for (uint32_t j = 0; j < outSz; ++j)
            {
                delta[j] *= ActivationDerivFromOutput(layerOutputs[static_cast<size_t>(l)][j], layer.activation);
            }

            // Accumulate weight + bias gradients: dW[j,i] += delta[j] * input[i]
            const uint32_t wOff = m_layerWeightOffsets[static_cast<size_t>(l)];
            const float* lInput = layerInputPtrs[static_cast<size_t>(l)];
            float* gW = m_gradients.data() + wOff;
            float* gB = gW + inSize * outSz;
            for (uint32_t j = 0; j < outSz; ++j)
            {
                const float dj = delta[j];
                float* gRow = gW + j * inSize;
                for (uint32_t i = 0; i < inSize; ++i)
                {
                    gRow[i] += dj * lInput[i];
                }
                gB[j] += dj;
            }

            // Propagate delta to previous layer (or the input if l == 0).
            if (l > 0 || outInputGradient)
            {
                thread_local std::vector<float> prevDelta;
                prevDelta.assign(inSize, 0.0f);
                const float* w = weights + wOff;
                for (uint32_t j = 0; j < outSz; ++j)
                {
                    const float dj = delta[j];
                    const float* row = w + j * inSize;
                    for (uint32_t i = 0; i < inSize; ++i)
                    {
                        prevDelta[i] += row[i] * dj;
                    }
                }
                if (l == 0)
                {
                    if (outInputGradient)
                    {
                        std::memcpy(outInputGradient, prevDelta.data(), inSize * sizeof(float));
                    }
                }
                else
                {
                    delta.swap(prevDelta);
                }
            }
        }

        return lossVal;
    }

    // =========================================================================
    // Optimizer steps
    // =========================================================================

    void Trainer::StepSGD(std::vector<float>& weights, const SGDConfig& cfg, float gradientScale)
    {
        if (weights.size() < m_totalParams)
        {
            return;
        }
        const float lr = cfg.learningRate;
        const float mom = cfg.momentum;
        const float wd = cfg.weightDecay;
        for (uint32_t i = 0; i < m_totalParams; ++i)
        {
            float g = m_gradients[i] * gradientScale + wd * weights[i];
            if (mom > 0.0f)
            {
                m_momentum[i] = mom * m_momentum[i] + g;
                g = m_momentum[i];
            }
            weights[i] -= lr * g;
        }
    }

    void Trainer::StepAdam(std::vector<float>& weights, const AdamConfig& cfg, float gradientScale)
    {
        if (weights.size() < m_totalParams)
        {
            return;
        }
        ++m_adamStep;
        const float b1 = cfg.beta1;
        const float b2 = cfg.beta2;
        const float eps = cfg.epsilon;
        const float lr = cfg.learningRate;
        const float wd = cfg.weightDecay;

        // Bias-correction terms.
        const float b1Pow = 1.0f - std::pow(b1, static_cast<float>(m_adamStep));
        const float b2Pow = 1.0f - std::pow(b2, static_cast<float>(m_adamStep));

        for (uint32_t i = 0; i < m_totalParams; ++i)
        {
            float g = m_gradients[i] * gradientScale;
            m_adamM[i] = b1 * m_adamM[i] + (1.0f - b1) * g;
            m_adamV[i] = b2 * m_adamV[i] + (1.0f - b2) * g * g;

            float mHat = m_adamM[i] / b1Pow;
            float vHat = m_adamV[i] / b2Pow;

            float update = lr * mHat / (std::sqrt(vHat) + eps);
            if (wd > 0.0f)
            {
                // Decoupled weight decay (AdamW).
                update += lr * wd * weights[i];
            }
            weights[i] -= update;
        }
    }

    // =========================================================================
    // Training loops
    // =========================================================================

    float Trainer::TrainEpoch(std::vector<float>& weights, const NetworkDesc& desc, const float* inputs,
                              const float* targets, uint32_t numSamples, LossType loss, const AdamConfig& cfg)
    {
        if (numSamples == 0)
        {
            return 0.0f;
        }
        const uint32_t inSize = desc.GetInputSize();
        const uint32_t outSize = desc.GetOutputSize();
        float total = 0.0f;
        for (uint32_t s = 0; s < numSamples; ++s)
        {
            ZeroGradients();
            total += AccumulateGradient(weights.data(), desc, inputs + s * inSize, targets + s * outSize, loss);
            StepAdam(weights, cfg, 1.0f);
        }
        return total / static_cast<float>(numSamples);
    }

    float Trainer::TrainSample(std::vector<float>& weights, const NetworkDesc& desc, const float* input,
                               const float* target, LossType loss, const AdamConfig& cfg)
    {
        ZeroGradients();
        float l = AccumulateGradient(weights.data(), desc, input, target, loss);
        StepAdam(weights, cfg, 1.0f);
        return l;
    }

    // =========================================================================
    // Numerical gradient check (tests only — very slow)
    // =========================================================================

    namespace
    {
        float EvaluateLoss(const NetworkDesc& desc, const std::vector<float>& weights, const float* input,
                           const float* target, LossType loss)
        {
            const uint32_t layerCount = static_cast<uint32_t>(desc.layers.size());
            std::vector<float> curr(desc.GetInputSize());
            std::memcpy(curr.data(), input, desc.GetInputSize() * sizeof(float));
            uint32_t offset = 0;
            for (uint32_t l = 0; l < layerCount; ++l)
            {
                const auto& layer = desc.layers[l];
                std::vector<float> next(layer.outputSize);
                const float* w = weights.data() + offset;
                const float* b = w + layer.inputSize * layer.outputSize;
                for (uint32_t j = 0; j < layer.outputSize; ++j)
                {
                    float sum = b[j];
                    const float* row = w + j * layer.inputSize;
                    for (uint32_t i = 0; i < layer.inputSize; ++i)
                    {
                        sum += row[i] * curr[i];
                    }
                    next[j] = ActivationForward(sum, layer.activation);
                }
                offset += layer.inputSize * layer.outputSize + layer.outputSize;
                curr = std::move(next);
            }
            return ComputeLoss(loss, curr.data(), target, desc.GetOutputSize(), nullptr);
        }
    } // namespace

    float Trainer::GradientCheck(const NetworkDesc& desc, const std::vector<float>& weights, const float* input,
                                 const float* target, LossType loss, float epsilon)
    {
        Trainer t;
        t.Initialize(desc);
        t.AccumulateGradient(weights.data(), desc, input, target, loss);
        const auto& analytical = t.GetGradients();

        std::vector<float> perturbed = weights;
        float maxRelErr = 0.0f;
        for (uint32_t i = 0; i < t.m_totalParams; ++i)
        {
            float orig = perturbed[i];
            perturbed[i] = orig + epsilon;
            float lossPlus = EvaluateLoss(desc, perturbed, input, target, loss);
            perturbed[i] = orig - epsilon;
            float lossMinus = EvaluateLoss(desc, perturbed, input, target, loss);
            perturbed[i] = orig;

            float numerical = (lossPlus - lossMinus) / (2.0f * epsilon);
            float denom = std::max(1e-6f, std::fabs(numerical) + std::fabs(analytical[i]));
            float relErr = std::fabs(numerical - analytical[i]) / denom;
            if (relErr > maxRelErr)
            {
                maxRelErr = relErr;
            }
        }
        return maxRelErr;
    }

} // namespace Spark::Graphics::Neural
