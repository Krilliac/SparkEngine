/**
 * @file NeuralInference.cpp
 * @brief GPU neural network inference engine implementation
 */

#include "NeuralInference.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace Spark::Graphics::Neural
{

    NeuralInferenceEngine& NeuralInferenceEngine::GetInstance()
    {
        static NeuralInferenceEngine instance;
        return instance;
    }

    bool NeuralInferenceEngine::Initialize(void* device, void* context)
    {
        std::lock_guard lock(m_mutex);

        if (m_initialized)
        {
            return true;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        m_device = static_cast<ID3D11Device*>(device);
        m_context = static_cast<ID3D11DeviceContext*>(context);

        if (m_device && m_context)
        {
            m_gpuAvailable = CompileComputeShader();
        }
#else
        (void)device;
        (void)context;
#endif

        m_initialized = true;
        return true;
    }

    void NeuralInferenceEngine::Shutdown()
    {
        std::lock_guard lock(m_mutex);

        if (!m_initialized)
        {
            return;
        }

        m_networks.clear();
        m_cpuWeights.clear();

#ifdef SPARK_PLATFORM_WINDOWS
        m_inferenceCS.Reset();
        m_constantBuffer.Reset();
        m_device = nullptr;
        m_context = nullptr;
#endif

        m_gpuAvailable = false;
        m_initialized = false;
    }

    NetworkHandle NeuralInferenceEngine::CreateNetwork(const NetworkDesc& desc)
    {
        std::lock_guard lock(m_mutex);

        if (!m_initialized || desc.layers.empty())
        {
            return NetworkHandle{};
        }

        if (desc.layers.size() > kMaxNetworkLayers)
        {
            return NetworkHandle{};
        }

        for (const auto& layer : desc.layers)
        {
            if (layer.inputSize > kMaxNeuronsPerLayer || layer.outputSize > kMaxNeuronsPerLayer)
            {
                return NetworkHandle{};
            }
        }

        uint32_t id = m_nextNetworkId++;
        GPUNetwork& network = m_networks[id];
        network.desc = desc;
        network.valid = true;

        // Build constant buffer data
        NeuralInferenceCB& cb = network.constantData;
        cb.layerCount = static_cast<uint32_t>(desc.layers.size());
        cb.batchSize = 0; // Set at dispatch time
        cb.inputSize = desc.GetInputSize();
        cb.outputSize = desc.GetOutputSize();

        uint32_t weightOffset = 0;
        for (uint32_t i = 0; i < cb.layerCount; ++i)
        {
            const auto& layer = desc.layers[i];
            cb.layerInputSizes[i] = layer.inputSize;
            cb.layerOutputSizes[i] = layer.outputSize;
            cb.layerActivations[i] = static_cast<uint32_t>(layer.activation);
            cb.layerWeightOffsets[i] = weightOffset;
            weightOffset += layer.inputSize * layer.outputSize + layer.outputSize;
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // Create weight structured buffer (will be filled by UploadWeights)
        if (m_device && m_gpuAvailable)
        {
            uint32_t totalParams = desc.GetTotalParameters();
            D3D11_BUFFER_DESC bufDesc{};
            bufDesc.ByteWidth = totalParams * sizeof(float);
            bufDesc.Usage = D3D11_USAGE_DEFAULT;
            bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufDesc.StructureByteStride = sizeof(float);

            HRESULT hr = m_device->CreateBuffer(&bufDesc, nullptr, &network.weightBuffer);
            if (SUCCEEDED(hr))
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                srvDesc.Buffer.NumElements = totalParams;

                m_device->CreateShaderResourceView(network.weightBuffer.Get(), &srvDesc, &network.weightSRV);
            }
        }
#endif

        ++m_networksCreated;
        return NetworkHandle{id};
    }

    bool NeuralInferenceEngine::UploadWeights(NetworkHandle handle, const std::vector<float>& weights)
    {
        std::lock_guard lock(m_mutex);

        auto it = m_networks.find(handle.id);
        if (it == m_networks.end() || !it->second.valid)
        {
            return false;
        }

        const auto& network = it->second;
        uint32_t expectedParams = network.desc.GetTotalParameters();
        if (weights.size() != expectedParams)
        {
            return false;
        }

        // Store CPU copy for CPU evaluation path
        m_cpuWeights[handle.id] = weights;

#ifdef SPARK_PLATFORM_WINDOWS
        // Upload to GPU buffer
        if (m_context && network.weightBuffer)
        {
            m_context->UpdateSubresource(network.weightBuffer.Get(), 0, nullptr, weights.data(), 0, 0);
        }
#endif

        return true;
    }

    void NeuralInferenceEngine::Evaluate(NetworkHandle handle, void* inputSRV, void* outputUAV, uint32_t batchSize)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        std::lock_guard lock(m_mutex);

        if (!m_gpuAvailable || !m_inferenceCS || !m_context)
        {
            return;
        }

        auto it = m_networks.find(handle.id);
        if (it == m_networks.end() || !it->second.valid)
        {
            return;
        }

        auto& network = it->second;

        // Update constant buffer with batch size
        network.constantData.batchSize = batchSize;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            std::memcpy(mapped.pData, &network.constantData, sizeof(NeuralInferenceCB));
            m_context->Unmap(m_constantBuffer.Get(), 0);
        }

        // Bind resources
        m_context->CSSetShader(m_inferenceCS.Get(), nullptr, 0);

        ID3D11Buffer* cbs[] = {m_constantBuffer.Get()};
        m_context->CSSetConstantBuffers(0, 1, cbs);

        auto* weightSRV = network.weightSRV.Get();
        auto* inputSRVTyped = static_cast<ID3D11ShaderResourceView*>(inputSRV);
        ID3D11ShaderResourceView* srvs[] = {weightSRV, inputSRVTyped};
        m_context->CSSetShaderResources(0, 2, srvs);

        auto* outputUAVTyped = static_cast<ID3D11UnorderedAccessView*>(outputUAV);
        ID3D11UnorderedAccessView* uavs[] = {outputUAVTyped};
        uint32_t initialCounts[] = {0};
        m_context->CSSetUnorderedAccessViews(0, 1, uavs, initialCounts);

        // Dispatch: one thread per sample
        uint32_t groupCount = (batchSize + kNeuralThreadGroupSize - 1) / kNeuralThreadGroupSize;
        m_context->Dispatch(groupCount, 1, 1);

        // Unbind
        ID3D11ShaderResourceView* nullSRVs[] = {nullptr, nullptr};
        m_context->CSSetShaderResources(0, 2, nullSRVs);
        ID3D11UnorderedAccessView* nullUAVs[] = {nullptr};
        m_context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
        m_context->CSSetShader(nullptr, nullptr, 0);

        ++m_totalDispatches;
#else
        (void)handle;
        (void)inputSRV;
        (void)outputUAV;
        (void)batchSize;
#endif
    }

    void NeuralInferenceEngine::EvaluateCPU(NetworkHandle handle, const float* input, float* output, uint32_t batchSize)
    {
        std::lock_guard lock(m_mutex);

        auto netIt = m_networks.find(handle.id);
        auto weightIt = m_cpuWeights.find(handle.id);
        if (netIt == m_networks.end() || weightIt == m_cpuWeights.end())
        {
            return;
        }

        const auto& desc = netIt->second.desc;
        const float* allWeights = weightIt->second.data();

        // Temporary buffers for intermediate activations
        std::vector<float> bufA(kMaxNeuronsPerLayer);
        std::vector<float> bufB(kMaxNeuronsPerLayer);

        for (uint32_t sample = 0; sample < batchSize; ++sample)
        {
            // Load input
            const float* sampleInput = input + sample * desc.GetInputSize();
            std::copy_n(sampleInput, desc.GetInputSize(), bufA.data());

            uint32_t weightOffset = 0;
            for (uint32_t layer = 0; layer < desc.layers.size(); ++layer)
            {
                const auto& layerDesc = desc.layers[layer];
                const float* weights = allWeights + weightOffset;
                const float* biases = weights + layerDesc.inputSize * layerDesc.outputSize;

                float* src = (layer % 2 == 0) ? bufA.data() : bufB.data();
                float* dst = (layer % 2 == 0) ? bufB.data() : bufA.data();

                EvaluateLayerCPU(src, dst, weights, biases, layerDesc.inputSize, layerDesc.outputSize,
                                 layerDesc.activation, 1);

                weightOffset += layerDesc.inputSize * layerDesc.outputSize + layerDesc.outputSize;
            }

            // Copy output: layer 0 (even) writes to B, layer 1 (odd) writes to A, etc.
            // After N layers, result is in B if N is odd, A if N is even.
            float* finalBuf = (desc.layers.size() % 2 != 0) ? bufB.data() : bufA.data();
            std::copy_n(finalBuf, desc.GetOutputSize(), output + sample * desc.GetOutputSize());
        }
    }

    void NeuralInferenceEngine::EvaluateLayerCPU(const float* input, float* output, const float* weights,
                                                 const float* biases, uint32_t inputSize, uint32_t outputSize,
                                                 ActivationType activation, uint32_t /*batchSize*/)
    {
        for (uint32_t j = 0; j < outputSize; ++j)
        {
            float sum = 0.0f;
            for (uint32_t i = 0; i < inputSize; ++i)
            {
                sum += weights[j * inputSize + i] * input[i];
            }
            sum += biases[j];

            switch (activation)
            {
            case ActivationType::ReLU:
                sum = std::max(0.0f, sum);
                break;
            case ActivationType::LeakyReLU:
                sum = sum > 0.0f ? sum : 0.01f * sum;
                break;
            case ActivationType::Sigmoid:
                sum = 1.0f / (1.0f + std::exp(-sum));
                break;
            case ActivationType::Tanh:
                sum = std::tanh(sum);
                break;
            case ActivationType::None:
                break;
            }

            output[j] = sum;
        }
    }

    void NeuralInferenceEngine::DestroyNetwork(NetworkHandle handle)
    {
        std::lock_guard lock(m_mutex);
        m_networks.erase(handle.id);
        m_cpuWeights.erase(handle.id);
    }

    const NetworkDesc* NeuralInferenceEngine::GetNetworkDesc(NetworkHandle handle) const
    {
        std::lock_guard lock(m_mutex);
        auto it = m_networks.find(handle.id);
        if (it == m_networks.end())
        {
            return nullptr;
        }
        return &it->second.desc;
    }

    bool NeuralInferenceEngine::CompileComputeShader()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        if (!m_device)
        {
            return false;
        }

        // Inline HLSL source for the neural inference compute shader
        static constexpr const char* kShaderSource = R"(
#define MAX_NEURONS 256
#define MAX_LAYERS  8

cbuffer NeuralCB : register(b0)
{
    uint g_LayerCount;
    uint g_BatchSize;
    uint g_InputSize;
    uint g_OutputSize;
    uint g_LayerInputSizes[MAX_LAYERS];
    uint g_LayerOutputSizes[MAX_LAYERS];
    uint g_LayerActivations[MAX_LAYERS];
    uint g_LayerWeightOffsets[MAX_LAYERS];
};

StructuredBuffer<float>   g_Weights : register(t0);
StructuredBuffer<float>   g_Input   : register(t1);
RWStructuredBuffer<float> g_Output  : register(u0);

float ApplyActivation(float x, uint activationType)
{
    switch (activationType)
    {
        case 0: return max(0.0f, x);
        case 1: return x > 0.0f ? x : 0.01f * x;
        case 2: return 1.0f / (1.0f + exp(-x));
        case 3: return tanh(x);
        default: return x;
    }
}

[numthreads(64, 1, 1)]
void CSNeuralForward(uint3 dtid : SV_DispatchThreadID)
{
    uint sampleIdx = dtid.x;
    if (sampleIdx >= g_BatchSize) return;

    float activationsA[MAX_NEURONS];
    float activationsB[MAX_NEURONS];

    uint inputOff = sampleIdx * g_InputSize;
    for (uint i = 0; i < g_InputSize; ++i)
        activationsA[i] = g_Input[inputOff + i];

    for (uint layer = 0; layer < g_LayerCount; ++layer)
    {
        uint inSz  = g_LayerInputSizes[layer];
        uint outSz = g_LayerOutputSizes[layer];
        uint act   = g_LayerActivations[layer];
        uint wOff  = g_LayerWeightOffsets[layer];
        bool readA = (layer % 2 == 0);

        for (uint j = 0; j < outSz; ++j)
        {
            float sum = 0.0f;
            uint rowOff = wOff + j * inSz;
            for (uint i = 0; i < inSz; ++i)
                sum += g_Weights[rowOff + i] * (readA ? activationsA[i] : activationsB[i]);
            sum += g_Weights[wOff + outSz * inSz + j];

            if (readA) activationsB[j] = ApplyActivation(sum, act);
            else       activationsA[j] = ApplyActivation(sum, act);
        }
    }

    uint outputOff = sampleIdx * g_OutputSize;
    bool finalInB = (g_LayerCount % 2 != 0);
    for (uint i = 0; i < g_OutputSize; ++i)
        g_Output[outputOff + i] = finalInB ? activationsB[i] : activationsA[i];
}
)";

        Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        HRESULT hr =
            D3DCompile(kShaderSource, std::strlen(kShaderSource), "NeuralInference.hlsl", nullptr, nullptr,
                       "CSNeuralForward", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shaderBlob, &errorBlob);

        if (FAILED(hr))
        {
            if (errorBlob)
            {
                std::fprintf(stderr, "[NeuralInference] Shader compile error: %s\n",
                             static_cast<const char*>(errorBlob->GetBufferPointer()));
            }
            return false;
        }

        hr = m_device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
                                           &m_inferenceCS);
        if (FAILED(hr))
        {
            return false;
        }

        // Create constant buffer
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(NeuralInferenceCB);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
        return SUCCEEDED(hr);
#else
        return false;
#endif
    }

    std::string NeuralInferenceEngine::Console_GetStatus() const
    {
        std::lock_guard lock(m_mutex);
        std::string status = "Neural Inference Engine:\n";
        status += "  Initialized: " + std::string(m_initialized ? "YES" : "NO") + "\n";
        status += "  GPU available: " + std::string(m_gpuAvailable ? "YES" : "NO") + "\n";
        status += "  Networks: " + std::to_string(m_networks.size()) + "\n";
        status += "  Total created: " + std::to_string(m_networksCreated) + "\n";
        status += "  Total dispatches: " + std::to_string(m_totalDispatches) + "\n";
        return status;
    }

} // namespace Spark::Graphics::Neural
