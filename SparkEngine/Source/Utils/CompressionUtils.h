/**
 * @file CompressionUtils.h
 * @brief Unified compression API supporting multiple algorithms (miniz deflate, zstd)
 *
 * Provides a simple Compress/Decompress interface that dispatches to the
 * appropriate backend based on the chosen compression method. This decouples
 * callers (SparkPak, SaveSystem, CrashHandler) from specific compression libs.
 */

#pragma once

#include <cstdint>
#include <vector>

namespace Spark
{

    /// @brief Compression algorithm selection.
    enum class CompressionMethod : uint8_t
    {
        None = 0,    ///< No compression (stored verbatim)
        Deflate = 1, ///< zlib/deflate via miniz (backward compat)
        Zstd = 2,    ///< Zstandard (faster decompression, better ratios)
    };

    /**
     * @brief Compress data using the specified method.
     * @param input       Source data to compress.
     * @param inputSize   Size of source data in bytes.
     * @param method      Compression algorithm to use.
     * @param level       Compression level (1-22 for zstd, 1-9 for deflate). 0 = default.
     * @return Compressed data, or empty vector on failure.
     */
    std::vector<uint8_t> CompressData(const uint8_t* input, size_t inputSize, CompressionMethod method, int level = 0);

    /**
     * @brief Decompress data using the specified method.
     * @param input          Compressed data.
     * @param inputSize      Size of compressed data.
     * @param originalSize   Expected decompressed size (must be known).
     * @param method         Compression algorithm that was used.
     * @return Decompressed data, or empty vector on failure.
     */
    std::vector<uint8_t> DecompressData(const uint8_t* input, size_t inputSize, size_t originalSize,
                                        CompressionMethod method);

    /// @brief Get the upper bound on compressed size for a given input size and method.
    size_t CompressBound(size_t inputSize, CompressionMethod method);

    /// @brief Check if a compression method is available at runtime.
    bool IsCompressionAvailable(CompressionMethod method);

    /// @brief Get human-readable name for a compression method.
    const char* GetCompressionMethodName(CompressionMethod method);

} // namespace Spark
