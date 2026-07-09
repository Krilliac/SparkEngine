/**
 * @file NeuralTextureCompressor.cpp
 * @brief Neural texture compression: CPU training + CPU/GPU decompression
 */

#include "NeuralTextureCompressor.h"
#include "CpuNeuralTraining.h"
#include "NeuralInference.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Spark::Graphics::Neural
{

    bool NeuralTextureCompressor::Initialize()
    {
        m_initialized = true;
        return true;
    }

    NetworkDesc NeuralTextureCompressor::BuildBlockNetwork(const NTCOptions& options) const
    {
        // Input size: 2 (u,v) + 4 * numFrequencies (sin/cos for u and v)
        uint32_t inputSize = 2 + 4 * options.positionalFrequencies;
        uint32_t outputSize = 4; // RGBA

        NetworkDesc desc;
        desc.name = "ntc_block";

        // Input → first hidden
        desc.layers.push_back({inputSize, options.neuronsPerLayer, ActivationType::ReLU});

        // Hidden layers
        for (uint32_t i = 1; i < options.hiddenLayers; ++i)
        {
            desc.layers.push_back({options.neuronsPerLayer, options.neuronsPerLayer, ActivationType::ReLU});
        }

        // Last hidden → output (sigmoid to clamp [0,1])
        desc.layers.push_back({options.neuronsPerLayer, outputSize, ActivationType::Sigmoid});

        return desc;
    }

    std::vector<float> NeuralTextureCompressor::EncodePosition(float u, float v, uint32_t numFrequencies) const
    {
        std::vector<float> encoded;
        encoded.reserve(2 + 4 * numFrequencies);

        encoded.push_back(u);
        encoded.push_back(v);

        constexpr float pi = 3.14159265358979f;
        for (uint32_t f = 0; f < numFrequencies; ++f)
        {
            float scale = std::pow(2.0f, static_cast<float>(f)) * pi;
            encoded.push_back(std::sin(scale * u));
            encoded.push_back(std::cos(scale * u));
            encoded.push_back(std::sin(scale * v));
            encoded.push_back(std::cos(scale * v));
        }

        return encoded;
    }

    std::vector<float> NeuralTextureCompressor::TrainBlock(const std::vector<uint8_t>& pixels, uint32_t blockSize,
                                                           const NetworkDesc& desc, const NTCOptions& options)
    {
        const uint32_t inputSize = desc.GetInputSize();
        const uint32_t outputSize = desc.GetOutputSize();
        const uint32_t numPixels = blockSize * blockSize;

        std::vector<float> weights;
        Trainer::InitializeWeightsXavier(desc, weights, 42u);

        // Pack inputs (positional-encoded UV) + targets (normalised RGBA) into
        // flat contiguous buffers so the training loop can iterate by stride.
        std::vector<float> inputs(static_cast<size_t>(numPixels) * inputSize);
        std::vector<float> targets(static_cast<size_t>(numPixels) * outputSize);
        for (uint32_t py = 0; py < blockSize; ++py)
        {
            for (uint32_t px = 0; px < blockSize; ++px)
            {
                const float u = (static_cast<float>(px) + 0.5f) / static_cast<float>(blockSize);
                const float v = (static_cast<float>(py) + 0.5f) / static_cast<float>(blockSize);
                const uint32_t idx = py * blockSize + px;

                auto encoded = EncodePosition(u, v, options.positionalFrequencies);
                std::copy(encoded.begin(), encoded.end(), inputs.begin() + static_cast<size_t>(idx) * inputSize);

                for (uint32_t c = 0; c < outputSize; ++c)
                {
                    targets[idx * outputSize + c] = static_cast<float>(pixels[idx * 4 + c]) / 255.0f;
                }
            }
        }

        uint32_t iterations = options.trainingIterations;
        if (iterations == 0)
        {
            switch (options.qualityLevel)
            {
            case 0:
                iterations = 200;
                break;
            case 1:
                iterations = 500;
                break;
            case 2:
                iterations = 1000;
                break;
            default:
                iterations = 2000;
                break;
            }
        }

        // Adam converges faster and more robustly than the previous vanilla-SGD
        // implementation at similar per-iteration cost, so we match or beat the
        // old quality with the same iteration budget.
        AdamConfig adam;
        adam.learningRate = options.learningRate > 0.0f ? options.learningRate : 1e-2f;

        Trainer trainer;
        trainer.Initialize(desc);

        for (uint32_t iter = 0; iter < iterations; ++iter)
        {
            // Mini-batch: accumulate gradient across all pixels in the block, then
            // one Adam step averaged by the batch size. Matches the previous
            // full-batch SGD semantics but with proper bias-corrected moments.
            trainer.ZeroGradients();
            for (uint32_t pixel = 0; pixel < numPixels; ++pixel)
            {
                trainer.AccumulateGradient(weights.data(), desc, &inputs[static_cast<size_t>(pixel) * inputSize],
                                           &targets[static_cast<size_t>(pixel) * outputSize], LossType::MSE);
            }
            trainer.StepAdam(weights, adam, 1.0f / static_cast<float>(numPixels));
        }

        return weights;
    }

    std::vector<uint8_t> NeuralTextureCompressor::DecodeBlockCPU(const std::vector<float>& weights,
                                                                 const NetworkDesc& desc, uint32_t blockSize,
                                                                 uint32_t numFrequencies)
    {
        auto& engine = NeuralInferenceEngine::GetInstance();

        // Create a temporary network for CPU evaluation
        auto handle = engine.CreateNetwork(desc);
        if (!handle.IsValid())
        {
            return {};
        }

        engine.UploadWeights(handle, weights);

        uint32_t numPixels = blockSize * blockSize;
        uint32_t inputSize = desc.GetInputSize();
        uint32_t outputSize = desc.GetOutputSize();

        // Build input batch (positional-encoded UVs for all pixels)
        std::vector<float> inputBatch(static_cast<size_t>(numPixels) * inputSize);
        for (uint32_t py = 0; py < blockSize; ++py)
        {
            for (uint32_t px = 0; px < blockSize; ++px)
            {
                float u = (static_cast<float>(px) + 0.5f) / static_cast<float>(blockSize);
                float v = (static_cast<float>(py) + 0.5f) / static_cast<float>(blockSize);
                auto encoded = EncodePosition(u, v, numFrequencies);

                uint32_t idx = py * blockSize + px;
                std::copy(encoded.begin(), encoded.end(), inputBatch.begin() + static_cast<size_t>(idx) * inputSize);
            }
        }

        // Evaluate
        std::vector<float> outputBatch(static_cast<size_t>(numPixels) * outputSize);
        engine.EvaluateCPU(handle, inputBatch.data(), outputBatch.data(), numPixels);

        engine.DestroyNetwork(handle);

        // Convert float output to uint8 RGBA
        std::vector<uint8_t> result(numPixels * 4);
        for (uint32_t i = 0; i < numPixels; ++i)
        {
            for (uint32_t c = 0; c < 4; ++c)
            {
                float val = std::clamp(outputBatch[i * outputSize + c], 0.0f, 1.0f);
                result[i * 4 + c] = static_cast<uint8_t>(val * 255.0f + 0.5f);
            }
        }

        return result;
    }

    NeuralCompressedTexture NeuralTextureCompressor::Compress(const uint8_t* rgba, uint32_t width, uint32_t height,
                                                              const NTCOptions& options)
    {
        NeuralCompressedTexture result;

        if (!rgba || width == 0 || height == 0)
        {
            return result;
        }

        uint32_t blockSize = options.blockSize;
        uint32_t blocksX = (width + blockSize - 1) / blockSize;
        uint32_t blocksY = (height + blockSize - 1) / blockSize;

        NetworkDesc blockDesc = BuildBlockNetwork(options);

        // Fill header
        auto& hdr = result.header;
        hdr.width = width;
        hdr.height = height;
        hdr.channels = 4;
        hdr.blockSize = blockSize;
        hdr.blocksX = blocksX;
        hdr.blocksY = blocksY;
        hdr.hiddenLayers = options.hiddenLayers;
        hdr.neuronsPerLayer = options.neuronsPerLayer;
        hdr.inputSize = blockDesc.GetInputSize();
        hdr.outputSize = blockDesc.GetOutputSize();
        hdr.positionalFrequencies = options.positionalFrequencies;
        hdr.weightsPerBlock = blockDesc.GetTotalParameters();
        hdr.totalBlocks = blocksX * blocksY;
        hdr.qualityLevel = options.qualityLevel;

        result.blockWeights.resize(hdr.totalBlocks);

        // Train each block
        for (uint32_t by = 0; by < blocksY; ++by)
        {
            for (uint32_t bx = 0; bx < blocksX; ++bx)
            {
                // Extract block pixels (with clamping at edges)
                std::vector<uint8_t> blockPixels(static_cast<size_t>(blockSize) * blockSize * 4);
                for (uint32_t py = 0; py < blockSize; ++py)
                {
                    for (uint32_t px = 0; px < blockSize; ++px)
                    {
                        uint32_t sx = std::min(bx * blockSize + px, width - 1);
                        uint32_t sy = std::min(by * blockSize + py, height - 1);
                        uint32_t srcIdx = (sy * width + sx) * 4;
                        uint32_t dstIdx = (py * blockSize + px) * 4;
                        std::memcpy(&blockPixels[dstIdx], &rgba[srcIdx], 4);
                    }
                }

                // Train block MLP
                uint32_t blockIdx = by * blocksX + bx;
                result.blockWeights[blockIdx] = TrainBlock(blockPixels, blockSize, blockDesc, options);
            }
        }

        m_texturesCompressed++;
        m_totalBytesInput += static_cast<uint64_t>(width) * height * 4;
        m_totalBytesOutput += result.GetCompressedSize();

        return result;
    }

    std::vector<uint8_t> NeuralTextureCompressor::DecompressCPU(const NeuralCompressedTexture& compressed)
    {
        const auto& hdr = compressed.header;
        if (hdr.totalBlocks == 0 || compressed.blockWeights.empty())
        {
            return {};
        }

        std::vector<uint8_t> result(static_cast<size_t>(hdr.width) * hdr.height * 4, 0);

        NetworkDesc desc = BuildBlockNetwork(NTCOptions{hdr.blockSize, hdr.hiddenLayers, hdr.neuronsPerLayer,
                                                        hdr.positionalFrequencies, hdr.qualityLevel, 0, 0.0f});

        for (uint32_t by = 0; by < hdr.blocksY; ++by)
        {
            for (uint32_t bx = 0; bx < hdr.blocksX; ++bx)
            {
                uint32_t blockIdx = by * hdr.blocksX + bx;
                auto blockPixels =
                    DecodeBlockCPU(compressed.blockWeights[blockIdx], desc, hdr.blockSize, hdr.positionalFrequencies);

                // Copy block pixels into output (with boundary clamping)
                for (uint32_t py = 0; py < hdr.blockSize; ++py)
                {
                    for (uint32_t px = 0; px < hdr.blockSize; ++px)
                    {
                        uint32_t dx = bx * hdr.blockSize + px;
                        uint32_t dy = by * hdr.blockSize + py;
                        if (dx >= hdr.width || dy >= hdr.height)
                        {
                            continue;
                        }

                        uint32_t srcIdx = (py * hdr.blockSize + px) * 4;
                        uint32_t dstIdx = (dy * hdr.width + dx) * 4;
                        std::memcpy(&result[dstIdx], &blockPixels[srcIdx], 4);
                    }
                }
            }
        }

        return result;
    }

    bool NeuralTextureCompressor::SaveNTEX(const NeuralCompressedTexture& tex, const std::string& path)
    {
        if (tex.blockWeights.empty())
        {
            return false;
        }

        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file)
        {
            return false;
        }

        std::fwrite(&tex.header, sizeof(NTEXHeader), 1, file);

        for (const auto& blockWeights : tex.blockWeights)
        {
            std::fwrite(blockWeights.data(), sizeof(float), blockWeights.size(), file);
        }

        std::fclose(file);
        return true;
    }

    NeuralCompressedTexture NeuralTextureCompressor::LoadNTEX(const std::string& path)
    {
        NeuralCompressedTexture result;

        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file)
        {
            return result;
        }

        if (std::fread(&result.header, sizeof(NTEXHeader), 1, file) != 1)
        {
            std::fclose(file);
            return NeuralCompressedTexture{};
        }

        if (result.header.magic != kNTEXMagic || result.header.version != kNTEXVersion)
        {
            std::fclose(file);
            return NeuralCompressedTexture{};
        }

        result.blockWeights.resize(result.header.totalBlocks);
        for (uint32_t i = 0; i < result.header.totalBlocks; ++i)
        {
            result.blockWeights[i].resize(result.header.weightsPerBlock);
            if (std::fread(result.blockWeights[i].data(), sizeof(float), result.header.weightsPerBlock, file) !=
                result.header.weightsPerBlock)
            {
                std::fclose(file);
                return NeuralCompressedTexture{};
            }
        }

        std::fclose(file);
        return result;
    }

    std::string NeuralTextureCompressor::Console_GetStatus() const
    {
        std::string status = "Neural Texture Compressor:\n";
        status += "  Initialized: " + std::string(m_initialized ? "YES" : "NO") + "\n";
        status += "  Textures compressed: " + std::to_string(m_texturesCompressed) + "\n";
        status += "  Total input: " + std::to_string(m_totalBytesInput / 1024) + " KB\n";
        status += "  Total output: " + std::to_string(m_totalBytesOutput / 1024) + " KB\n";
        if (m_totalBytesInput > 0)
        {
            float ratio = static_cast<float>(m_totalBytesOutput) / static_cast<float>(m_totalBytesInput);
            status += "  Avg ratio: " + std::to_string(ratio) + "\n";
        }
        return status;
    }

} // namespace Spark::Graphics::Neural
