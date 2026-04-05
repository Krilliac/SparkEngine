/**
 * @file NeuralInference.h
 * @brief Lightweight GPU neural network inference engine (compute shader-based)
 * @author Spark Engine Team
 * @date 2026
 *
 * Evaluates small MLPs entirely on the GPU using compute shaders and
 * structured buffers. No external ML framework dependency — weights are
 * stored in GPU buffers and a single compute dispatch evaluates a batch
 * of inputs through all layers.
 *
 * ## Usage
 * @code
 *   auto& engine = NeuralInferenceEngine::GetInstance();
 *   engine.Initialize(device, context);
 *
 *   NetworkDesc desc;
 *   desc.name = "MyMLP";
 *   desc.layers = {{2, 16, ActivationType::ReLU},
 *                   {16, 16, ActivationType::ReLU},
 *                   {16, 4, ActivationType::Sigmoid}};
 *   auto handle = engine.CreateNetwork(desc);
 *   engine.UploadWeights(handle, trainedWeights);
 *   engine.Evaluate(handle, inputBuf, outputBuf, batchSize);
 * @endcode
 *
 * @see NeuralTypes.h, NeuralWeights.h
 */

#pragma once

#include "NeuralTypes.h"
#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics::Neural
{

    /**
     * @brief GPU-resident network instance with its weight buffers.
     */
    struct GPUNetwork
    {
        NetworkDesc desc;
        NeuralInferenceCB constantData{};
        bool valid = false;

#ifdef SPARK_PLATFORM_WINDOWS
        Microsoft::WRL::ComPtr<ID3D11Buffer> weightBuffer; ///< Structured buffer with weights+biases
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> weightSRV;
#endif
    };

    /**
     * @brief Lightweight GPU MLP inference engine.
     *
     * Singleton — accessed via GetInstance(). Manages compute shader
     * compilation, weight buffer allocation, and batched dispatch.
     * Falls back to CPU evaluation when no GPU is available (NullRHI / headless).
     */
    class NeuralInferenceEngine
    {
      public:
        static NeuralInferenceEngine& GetInstance();

        /**
         * @brief Initialize the inference engine.
         * @param device D3D11 device (nullptr for CPU-only mode)
         * @param context D3D11 device context (nullptr for CPU-only mode)
         * @return true on success
         */
        bool Initialize(void* device = nullptr, void* context = nullptr);

        /** @brief Shut down and release all GPU resources. */
        void Shutdown();

        /** @brief Check if the engine is initialized. */
        bool IsInitialized() const { return m_initialized; }

        /** @brief Check if GPU inference is available (vs CPU fallback). */
        bool IsGPUAvailable() const { return m_gpuAvailable; }

        /**
         * @brief Create a GPU-resident network from an architecture descriptor.
         * @param desc Network architecture
         * @return Handle to the created network (INVALID on failure)
         */
        NetworkHandle CreateNetwork(const NetworkDesc& desc);

        /**
         * @brief Upload trained weights to a network's GPU buffers.
         * @param handle Network handle
         * @param weights Float32 weight data (must match desc.GetTotalParameters())
         * @return true on success
         */
        bool UploadWeights(NetworkHandle handle, const std::vector<float>& weights);

        /**
         * @brief Evaluate a network on a batch of inputs (GPU compute dispatch).
         *
         * On GPU: binds weight SRV + input SRV, dispatches compute shader,
         * writes results to output UAV.
         * On CPU: runs a reference MLP forward pass.
         *
         * @param handle Network handle
         * @param inputSRV Input structured buffer SRV (batchSize * inputSize floats)
         * @param outputUAV Output structured buffer UAV (batchSize * outputSize floats)
         * @param batchSize Number of input samples
         */
        void Evaluate(NetworkHandle handle, void* inputSRV, void* outputUAV, uint32_t batchSize);

        /**
         * @brief CPU-only forward pass (reference implementation / headless fallback).
         * @param handle Network handle
         * @param input Input data (batchSize * inputSize floats)
         * @param output Output data (batchSize * outputSize floats)
         * @param batchSize Number of input samples
         */
        void EvaluateCPU(NetworkHandle handle, const float* input, float* output, uint32_t batchSize);

        /**
         * @brief Destroy a network and free its GPU resources.
         * @param handle Network handle
         */
        void DestroyNetwork(NetworkHandle handle);

        /** @brief Get the network descriptor for a handle. */
        const NetworkDesc* GetNetworkDesc(NetworkHandle handle) const;

        /** @brief Console status string. */
        std::string Console_GetStatus() const;

      private:
        NeuralInferenceEngine() = default;
        ~NeuralInferenceEngine() { Shutdown(); }
        NeuralInferenceEngine(const NeuralInferenceEngine&) = delete;
        NeuralInferenceEngine& operator=(const NeuralInferenceEngine&) = delete;

        bool CompileComputeShader();
        void EvaluateLayerCPU(const float* input, float* output, const float* weights, const float* biases,
                              uint32_t inputSize, uint32_t outputSize, ActivationType activation, uint32_t batchSize);

        bool m_initialized = false;
        bool m_gpuAvailable = false;

        mutable std::mutex m_mutex;
        uint32_t m_nextNetworkId = 0;
        std::unordered_map<uint32_t, GPUNetwork> m_networks;

        // CPU-side weight storage for CPU evaluation path
        std::unordered_map<uint32_t, std::vector<float>> m_cpuWeights;

#ifdef SPARK_PLATFORM_WINDOWS
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_inferenceCS;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
#endif

        // Stats
        uint32_t m_networksCreated = 0;
        uint32_t m_totalDispatches = 0;
    };

} // namespace Spark::Graphics::Neural
