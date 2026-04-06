/**
 * @file CompressionUtils.cpp
 * @brief Unified compression implementation dispatching to miniz and zstd
 */

#include "CompressionUtils.h"

#ifdef SPARK_MINIZ_AVAILABLE
#include <miniz.h>
#endif

#ifdef SPARK_ZSTD_AVAILABLE
#include <zstd.h>
#endif

namespace Spark
{

    std::vector<uint8_t> CompressData(const uint8_t* input, size_t inputSize, CompressionMethod method, int level)
    {
        if (!input || inputSize == 0)
            return {};

        switch (method)
        {
        case CompressionMethod::None:
            return {input, input + inputSize};

        case CompressionMethod::Deflate:
        {
#ifdef SPARK_MINIZ_AVAILABLE
            mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(inputSize));
            std::vector<uint8_t> output(bound);
            mz_ulong destLen = bound;
            int compLevel = (level > 0) ? level : MZ_DEFAULT_COMPRESSION;
            if (mz_compress2(output.data(), &destLen, input, static_cast<mz_ulong>(inputSize), compLevel) == MZ_OK)
            {
                output.resize(destLen);
                return output;
            }
#endif
            return {};
        }

        case CompressionMethod::Zstd:
        {
#ifdef SPARK_ZSTD_AVAILABLE
            size_t bound = ZSTD_compressBound(inputSize);
            std::vector<uint8_t> output(bound);
            int compLevel = (level > 0) ? level : 3; // zstd default level
            size_t result = ZSTD_compress(output.data(), bound, input, inputSize, compLevel);
            if (!ZSTD_isError(result))
            {
                output.resize(result);
                return output;
            }
#endif
            return {};
        }
        }

        return {};
    }

    std::vector<uint8_t> DecompressData(const uint8_t* input, size_t inputSize, size_t originalSize,
                                        CompressionMethod method)
    {
        if (!input || inputSize == 0 || originalSize == 0)
            return {};

        switch (method)
        {
        case CompressionMethod::None:
            return {input, input + inputSize};

        case CompressionMethod::Deflate:
        {
#ifdef SPARK_MINIZ_AVAILABLE
            std::vector<uint8_t> output(originalSize);
            mz_ulong destLen = static_cast<mz_ulong>(originalSize);
            if (mz_uncompress(output.data(), &destLen, input, static_cast<mz_ulong>(inputSize)) == MZ_OK)
            {
                return output;
            }
#endif
            return {};
        }

        case CompressionMethod::Zstd:
        {
#ifdef SPARK_ZSTD_AVAILABLE
            std::vector<uint8_t> output(originalSize);
            size_t result = ZSTD_decompress(output.data(), originalSize, input, inputSize);
            if (!ZSTD_isError(result))
            {
                output.resize(result);
                return output;
            }
#endif
            return {};
        }
        }

        return {};
    }

    size_t CompressBound(size_t inputSize, CompressionMethod method)
    {
        switch (method)
        {
        case CompressionMethod::None:
            return inputSize;
        case CompressionMethod::Deflate:
#ifdef SPARK_MINIZ_AVAILABLE
            return mz_compressBound(static_cast<mz_ulong>(inputSize));
#else
            return inputSize;
#endif
        case CompressionMethod::Zstd:
#ifdef SPARK_ZSTD_AVAILABLE
            return ZSTD_compressBound(inputSize);
#else
            return inputSize;
#endif
        }
        return inputSize;
    }

    bool IsCompressionAvailable(CompressionMethod method)
    {
        switch (method)
        {
        case CompressionMethod::None:
            return true;
        case CompressionMethod::Deflate:
#ifdef SPARK_MINIZ_AVAILABLE
            return true;
#else
            return false;
#endif
        case CompressionMethod::Zstd:
#ifdef SPARK_ZSTD_AVAILABLE
            return true;
#else
            return false;
#endif
        }
        return false;
    }

    const char* GetCompressionMethodName(CompressionMethod method)
    {
        switch (method)
        {
        case CompressionMethod::None:
            return "None";
        case CompressionMethod::Deflate:
            return "Deflate";
        case CompressionMethod::Zstd:
            return "Zstd";
        }
        return "Unknown";
    }

} // namespace Spark
