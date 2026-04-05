/**
 * @file NeuralTextureCompressor.h
 * @brief Neural texture compression: encode textures as tiny MLPs per block
 * @author Spark Engine Team
 * @date 2026
 *
 * Compresses textures by training a small MLP per block that maps (u,v) →
 * (r,g,b,a). The network weights become the compressed representation.
 * Decompression evaluates the MLP on the GPU via compute shader to
 * reconstruct the original pixels.
 *
 * ## Usage
 * @code
 *   NeuralTextureCompressor ntc;
 *   ntc.Initialize();
 *
 *   NTCOptions opts;
 *   opts.qualityLevel = 2;
 *   auto compressed = ntc.Compress(rgba, 512, 512, opts);
 *   ntc.SaveNTEX(compressed, "texture.ntex");
 *
 *   auto loaded = ntc.LoadNTEX("texture.ntex");
 *   auto decoded = ntc.DecompressCPU(loaded); // CPU decode
 * @endcode
 *
 * @see NeuralInference.h, NeuralTextureFormat.h, TextureCompressor.h
 */

#pragma once

#include "NeuralTextureFormat.h"
#include "NeuralTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics::Neural
{

    /** @brief Compression quality/speed options for NTC. */
    struct NTCOptions
    {
        uint32_t blockSize = kDefaultNTCBlockSize; ///< Block edge size in pixels
        uint32_t hiddenLayers = 2;                 ///< Hidden layers per block MLP
        uint32_t neuronsPerLayer = 32;             ///< Neurons per hidden layer
        uint32_t positionalFrequencies = 4;        ///< Positional encoding bands
        uint32_t qualityLevel = 2;                 ///< 0=fast, 1=medium, 2=high, 3=best
        uint32_t trainingIterations = 0;           ///< 0 = auto (based on quality)
        float learningRate = 0.01f;                ///< SGD learning rate
    };

    /**
     * @brief In-memory neural-compressed texture.
     *
     * Contains header metadata + per-block weight arrays.
     */
    struct NeuralCompressedTexture
    {
        NTEXHeader header;
        std::vector<std::vector<float>> blockWeights; ///< Per-block MLP weights

        /** @brief Total compressed size in bytes (weights only). */
        size_t GetCompressedSize() const
        {
            if (blockWeights.empty())
            {
                return 0;
            }
            return blockWeights.size() * blockWeights[0].size() * sizeof(float);
        }

        /** @brief Original uncompressed size in bytes. */
        size_t GetOriginalSize() const { return static_cast<size_t>(header.width) * header.height * header.channels; }

        /** @brief Compression ratio (compressed / original). */
        float GetCompressionRatio() const
        {
            size_t orig = GetOriginalSize();
            return orig > 0 ? static_cast<float>(GetCompressedSize()) / static_cast<float>(orig) : 1.0f;
        }
    };

    /**
     * @brief Neural texture compression engine.
     *
     * Compresses RGBA textures using per-block MLPs. Training happens on
     * the CPU (simple SGD). Decompression can run on CPU or GPU.
     */
    class NeuralTextureCompressor
    {
      public:
        NeuralTextureCompressor() = default;

        /** @brief Initialize (loads inference engine reference). */
        bool Initialize();

        /**
         * @brief Compress a texture using neural block encoding.
         * @param rgba RGBA pixel data (row-major, 4 bytes per pixel)
         * @param width Texture width
         * @param height Texture height
         * @param options Compression settings
         * @return Neural-compressed texture
         */
        NeuralCompressedTexture Compress(const uint8_t* rgba, uint32_t width, uint32_t height,
                                         const NTCOptions& options = {});

        /**
         * @brief Decompress a neural texture back to RGBA on the CPU.
         * @param compressed The compressed texture
         * @return RGBA pixel data (width * height * 4 bytes)
         */
        std::vector<uint8_t> DecompressCPU(const NeuralCompressedTexture& compressed);

        /**
         * @brief Save a neural-compressed texture to a .ntex file.
         * @param tex Compressed texture
         * @param path Output file path
         * @return true on success
         */
        bool SaveNTEX(const NeuralCompressedTexture& tex, const std::string& path);

        /**
         * @brief Load a neural-compressed texture from a .ntex file.
         * @param path Input file path
         * @return Loaded texture (empty on failure)
         */
        NeuralCompressedTexture LoadNTEX(const std::string& path);

        /** @brief Console status string. */
        std::string Console_GetStatus() const;

      private:
        /** @brief Build the MLP architecture for a block. */
        NetworkDesc BuildBlockNetwork(const NTCOptions& options) const;

        /** @brief Compute positional encoding for UV coordinates. */
        std::vector<float> EncodePosition(float u, float v, uint32_t numFrequencies) const;

        /**
         * @brief Train a single block MLP to fit pixel data.
         * @param pixels Block pixel data (RGBA, blockSize * blockSize * 4)
         * @param blockSize Pixels per block edge
         * @param desc Network architecture
         * @param options Training options
         * @return Trained weights
         */
        std::vector<float> TrainBlock(const std::vector<uint8_t>& pixels, uint32_t blockSize, const NetworkDesc& desc,
                                      const NTCOptions& options);

        /** @brief Evaluate a block MLP for all pixels (CPU). */
        std::vector<uint8_t> DecodeBlockCPU(const std::vector<float>& weights, const NetworkDesc& desc,
                                            uint32_t blockSize, uint32_t numFrequencies);

        bool m_initialized = false;
        uint32_t m_texturesCompressed = 0;
        uint64_t m_totalBytesInput = 0;
        uint64_t m_totalBytesOutput = 0;
    };

} // namespace Spark::Graphics::Neural
