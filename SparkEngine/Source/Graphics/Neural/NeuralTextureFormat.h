/**
 * @file NeuralTextureFormat.h
 * @brief .ntex binary file format for neural-compressed textures
 * @author Spark Engine Team
 * @date 2026
 *
 * ## File Layout
 * ```
 * [NTEXHeader]                      — magic, version, dimensions, architecture
 * [float32] * weightsPerBlock       — block 0 weights (row-major, all layers)
 * [float32] * weightsPerBlock       — block 1 weights
 * ...                               — one weight blob per block
 * ```
 */

#pragma once

#include <cstdint>

namespace Spark::Graphics::Neural
{

    /** @brief Magic number for .ntex files ("SNTX" in little-endian). */
    static constexpr uint32_t kNTEXMagic = 0x58544E53; // 'S','N','T','X'

    /** @brief Current .ntex format version. */
    static constexpr uint32_t kNTEXVersion = 1;

    /** @brief Default block size for neural texture compression. */
    static constexpr uint32_t kDefaultNTCBlockSize = 16;

    /**
     * @brief Binary header for .ntex files.
     *
     * All fields are little-endian uint32 for simplicity.
     */
    struct NTEXHeader
    {
        uint32_t magic = kNTEXMagic;
        uint32_t version = kNTEXVersion;

        // Texture dimensions
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t channels = 4; ///< Output channels (3=RGB, 4=RGBA)

        // Block layout
        uint32_t blockSize = kDefaultNTCBlockSize; ///< Pixels per block edge (e.g., 16)
        uint32_t blocksX = 0;                      ///< Number of blocks horizontally
        uint32_t blocksY = 0;                      ///< Number of blocks vertically

        // Network architecture (same for all blocks)
        uint32_t hiddenLayers = 2;     ///< Number of hidden layers
        uint32_t neuronsPerLayer = 32; ///< Neurons in each hidden layer
        uint32_t inputSize = 0;        ///< MLP input size (with positional encoding)
        uint32_t outputSize = 4;       ///< MLP output size (channels)

        // Positional encoding
        uint32_t positionalFrequencies = 4; ///< Number of sin/cos frequency bands

        // Per-block weight count and total
        uint32_t weightsPerBlock = 0; ///< Total float32 weights per block MLP
        uint32_t totalBlocks = 0;     ///< blocksX * blocksY

        // Quality metadata
        uint32_t qualityLevel = 2; ///< 0=fast, 1=medium, 2=high, 3=best
        uint32_t reserved[3] = {}; ///< Padding for future use
    };

} // namespace Spark::Graphics::Neural
