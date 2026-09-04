/**
 * @file TextureBlockCompression.h
 * @brief Block compression algorithms for BC1 and BC7 texture formats
 *
 * Pure-math helpers extracted from TextureCompressor so the compressor
 * file stays under the 500-line threshold. Each function encodes a single
 * 4x4 RGBA block into the target format.
 */

#pragma once

#include <cstdint>

namespace Spark::Graphics
{

    /**
     * @brief Compress a 4x4 RGBA block into BC1 (DXT1) format
     * @param block4x4 64 bytes of RGBA pixel data (4x4 texels, 4 bytes each)
     * @param output 8 bytes of BC1 compressed output
     */
    void CompressBlockBC1(const uint8_t* block4x4, uint8_t* output);

    /**
     * @brief Compress a 4x4 RGBA block into BC7 mode 6 format
     * @param block4x4 64 bytes of RGBA pixel data (4x4 texels, 4 bytes each)
     * @param output 16 bytes of BC7 compressed output
     */
    void CompressBlockBC7(const uint8_t* block4x4, uint8_t* output);

} // namespace Spark::Graphics
