/**
 * @file NeuralTypes.h
 * @brief Shared types for the neural rendering subsystem
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines network architecture descriptors, activation functions, handles,
 * and GPU-compatible data layouts used by NeuralInference, NeuralTexture-
 * Compressor, NeuralRadianceCache, and NeuralPostProcessing.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics::Neural
{

    // =========================================================================
    // Activation functions
    // =========================================================================

    enum class ActivationType : uint8_t
    {
        ReLU,      ///< max(0, x)
        LeakyReLU, ///< x > 0 ? x : 0.01*x
        Sigmoid,   ///< 1 / (1 + exp(-x))
        Tanh,      ///< tanh(x)
        None       ///< Identity (linear output)
    };

    // =========================================================================
    // Layer descriptor
    // =========================================================================

    /** @brief Describes a single fully-connected layer in an MLP. */
    struct LayerDesc
    {
        uint32_t inputSize = 0;                           ///< Number of input neurons
        uint32_t outputSize = 0;                          ///< Number of output neurons
        ActivationType activation = ActivationType::ReLU; ///< Activation after this layer
    };

    // =========================================================================
    // Network descriptor
    // =========================================================================

    /** @brief Describes a complete MLP architecture (layers + metadata). */
    struct NetworkDesc
    {
        std::string name;               ///< Debug name
        std::vector<LayerDesc> layers;  ///< Ordered list of layers
        bool useFloat16Weights = false; ///< Pack weights as float16 on GPU

        /** @brief Total number of trainable parameters (weights + biases). */
        uint32_t GetTotalParameters() const
        {
            uint32_t total = 0;
            for (const auto& layer : layers)
            {
                total += layer.inputSize * layer.outputSize; // weights
                total += layer.outputSize;                   // biases
            }
            return total;
        }

        /** @brief Total weight data size in bytes (float32). */
        size_t GetWeightDataSize() const { return static_cast<size_t>(GetTotalParameters()) * sizeof(float); }

        /** @brief Input size of the first layer. */
        uint32_t GetInputSize() const { return layers.empty() ? 0 : layers.front().inputSize; }

        /** @brief Output size of the last layer. */
        uint32_t GetOutputSize() const { return layers.empty() ? 0 : layers.back().outputSize; }
    };

    // =========================================================================
    // Network handle
    // =========================================================================

    /** @brief Opaque handle to a GPU-resident neural network. */
    struct NetworkHandle
    {
        static constexpr uint32_t INVALID = ~0u;

        uint32_t id = INVALID;

        constexpr bool IsValid() const { return id != INVALID; }
        constexpr bool operator==(const NetworkHandle& other) const { return id == other.id; }
        constexpr bool operator!=(const NetworkHandle& other) const { return id != other.id; }
    };

    // =========================================================================
    // GPU constant buffer layout for neural inference shader
    // =========================================================================

    /** @brief Maximum layers supported by a single dispatch (shader constant). */
    static constexpr uint32_t kMaxNetworkLayers = 8;

    /** @brief Maximum neurons per layer (shader shared memory constraint). */
    static constexpr uint32_t kMaxNeuronsPerLayer = 256;

    /** @brief Thread group size for neural inference compute shader. */
    static constexpr uint32_t kNeuralThreadGroupSize = 64;

    /**
     * @brief GPU constant buffer matching the HLSL cbuffer layout.
     *
     * Packed as 16-byte aligned for D3D11 constant buffer rules.
     */
    struct alignas(16) NeuralInferenceCB
    {
        uint32_t layerCount; ///< Number of layers
        uint32_t batchSize;  ///< Number of input samples
        uint32_t inputSize;  ///< First layer input dimension
        uint32_t outputSize; ///< Last layer output dimension

        uint32_t layerInputSizes[kMaxNetworkLayers];    ///< Per-layer input sizes
        uint32_t layerOutputSizes[kMaxNetworkLayers];   ///< Per-layer output sizes
        uint32_t layerActivations[kMaxNetworkLayers];   ///< Per-layer activation type
        uint32_t layerWeightOffsets[kMaxNetworkLayers]; ///< Byte offset into weight buffer
    };

} // namespace Spark::Graphics::Neural
