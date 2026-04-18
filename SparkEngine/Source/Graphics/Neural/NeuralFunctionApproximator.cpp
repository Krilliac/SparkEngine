/**
 * @file NeuralFunctionApproximator.cpp
 * @brief Implementation of the learned CPU function facade
 */

#include "NeuralFunctionApproximator.h"

#include <algorithm>
#include <cstring>

namespace Spark::Graphics::Neural
{

    NeuralFunctionApproximator::NeuralFunctionApproximator() : m_trainer(std::make_unique<Trainer>()) {}

    NeuralFunctionApproximator::~NeuralFunctionApproximator() = default;

    NeuralFunctionApproximator::NeuralFunctionApproximator(NeuralFunctionApproximator&&) noexcept = default;
    NeuralFunctionApproximator& NeuralFunctionApproximator::operator=(NeuralFunctionApproximator&&) noexcept = default;

    void NeuralFunctionApproximator::Configure(uint32_t inputSize, uint32_t outputSize,
                                               const std::vector<uint32_t>& hiddenSizes, uint32_t seed)
    {
        m_desc.name = "NFA";
        m_desc.layers.clear();

        uint32_t prev = inputSize;
        for (uint32_t h : hiddenSizes)
        {
            if (h == 0)
            {
                continue;
            }
            m_desc.layers.push_back({prev, h, ActivationType::ReLU});
            prev = h;
        }
        m_desc.layers.push_back({prev, outputSize, ActivationType::None});

        Trainer::InitializeWeightsXavier(m_desc, m_weights, seed);
        m_trainer->Initialize(m_desc);
        m_layoutDirty = true;

        CpuNeuralInference::Initialize();
    }

    void NeuralFunctionApproximator::SetOutputActivation(ActivationType activation)
    {
        if (m_desc.layers.empty())
        {
            return;
        }
        m_desc.layers.back().activation = activation;
        // Changing the activation doesn't change the weight layout, only
        // forward/backward math — no need to mark layout dirty.
    }

    void NeuralFunctionApproximator::RebuildLayoutIfNeeded() const
    {
        if (!m_layoutDirty)
        {
            return;
        }
        m_cachedLayout = CpuNeuralInference::PrepareWeights(m_desc, m_weights.data());
        m_layoutDirty = false;
    }

    float NeuralFunctionApproximator::TrainOffline(const float* inputs, const float* targets, uint32_t numSamples,
                                                   uint32_t epochs, LossType loss)
    {
        if (!IsConfigured() || numSamples == 0 || epochs == 0 || !inputs || !targets)
        {
            return 0.0f;
        }

        float finalLoss = 0.0f;
        for (uint32_t e = 0; e < epochs; ++e)
        {
            finalLoss = m_trainer->TrainEpoch(m_weights, m_desc, inputs, targets, numSamples, loss, m_adamConfig);
        }
        MarkWeightsChanged();
        return finalLoss;
    }

    float NeuralFunctionApproximator::TrainIncremental(const float* input, const float* target, LossType loss)
    {
        if (!IsConfigured() || !input || !target)
        {
            return 0.0f;
        }
        float l = m_trainer->TrainSample(m_weights, m_desc, input, target, loss, m_adamConfig);
        MarkWeightsChanged();
        return l;
    }

    void NeuralFunctionApproximator::Query(const float* input, float* output) const
    {
        if (!IsConfigured() || !input || !output)
        {
            return;
        }
        RebuildLayoutIfNeeded();
        CpuNeuralInference::EvaluateBatch(m_cachedLayout, input, output, 1);
    }

    float NeuralFunctionApproximator::QueryScalar(float x0, float x1) const
    {
        if (GetInputSize() != 2 || GetOutputSize() != 1)
        {
            return 0.0f;
        }
        const float in[2] = {x0, x1};
        float out = 0.0f;
        Query(in, &out);
        return out;
    }

    float NeuralFunctionApproximator::QueryScalar(float x) const
    {
        if (GetInputSize() != 1 || GetOutputSize() != 1)
        {
            return 0.0f;
        }
        float out = 0.0f;
        Query(&x, &out);
        return out;
    }

    bool NeuralFunctionApproximator::Save(const std::string& path, bool includeOptimizerState) const
    {
        if (!IsConfigured())
        {
            return false;
        }
        TrainedNetwork net;
        net.desc = m_desc;
        net.weights = m_weights;

        if (includeOptimizerState)
        {
            // StepAdam() keeps its own moment buffers; the trainer exposes gradients
            // but not its internal optimizer state. Expose first/second moments
            // via a friend? For now, skip — reserved for a follow-up extension
            // when resume-training from disk becomes a real requirement.
            // Leave kind = None so the file is compact and v1-compat.
            net.optimizer.kind = OptimizerKind::None;
        }

        return SaveWeights(net, path);
    }

    bool NeuralFunctionApproximator::Load(const std::string& path)
    {
        TrainedNetwork net = LoadWeights(path);
        if (net.desc.layers.empty() || net.weights.empty())
        {
            return false;
        }
        m_desc = net.desc;
        m_weights = std::move(net.weights);
        m_trainer->Initialize(m_desc);
        m_layoutDirty = true;
        CpuNeuralInference::Initialize();
        return true;
    }

} // namespace Spark::Graphics::Neural
