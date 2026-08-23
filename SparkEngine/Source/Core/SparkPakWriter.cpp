/**
 * @file SparkPakWriter.cpp
 * @brief SparkPakWriter implementation — builds .spk archive files
 */

#include "SparkPakWriter.h"

#include "Utils/LogMacros.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef SPARK_MINIZ_AVAILABLE
#include <miniz.h>
#endif

#ifdef SPARK_ZSTD_AVAILABLE
#include <zstd.h>
#endif

// Use 64-bit file offset functions on Windows (long is 32-bit on MSVC)
#ifdef _WIN32
#define PAK_FSEEK(f, off, whence) _fseeki64((f), static_cast<int64_t>(off), (whence))
#define PAK_FTELL(f) static_cast<uint64_t>(_ftelli64((f)))
#else
#define PAK_FSEEK(f, off, whence) std::fseek((f), static_cast<long>(off), (whence))
#define PAK_FTELL(f) static_cast<uint64_t>(std::ftell((f)))
#endif

namespace Spark
{

    // =========================================================================
    // AddFile
    // =========================================================================

    void SparkPakWriter::AddFile(const std::string& virtualPath, const std::vector<uint8_t>& data, bool compress)
    {
        StagedFile staged;
        staged.virtualPath = virtualPath;
        staged.originalData = data;

        if (compress && !data.empty())
        {
            bool compressed_ok = false;

            // Try zstd first (faster decompression, better ratios)
#ifdef SPARK_ZSTD_AVAILABLE
            {
                size_t compBound = ZSTD_compressBound(data.size());
                staged.compressedData.resize(compBound);
                size_t result = ZSTD_compress(staged.compressedData.data(), compBound, data.data(), data.size(), 3);
                if (!ZSTD_isError(result))
                {
                    staged.compressedData.resize(result);
                    if (result < data.size() * 95 / 100)
                    {
                        staged.compression = PakCompression::Zstd;
                        compressed_ok = true;
                    }
                }
            }
#endif

            // Fall back to deflate if zstd unavailable or didn't help
#ifdef SPARK_MINIZ_AVAILABLE
            if (!compressed_ok)
            {
                mz_ulong compBound = mz_compressBound(static_cast<mz_ulong>(data.size()));
                staged.compressedData.resize(compBound);
                mz_ulong destLen = compBound;
                if (mz_compress(staged.compressedData.data(), &destLen, data.data(),
                                static_cast<mz_ulong>(data.size())) == MZ_OK)
                {
                    staged.compressedData.resize(destLen);
                    if (destLen < data.size() * 95 / 100)
                    {
                        staged.compression = PakCompression::Deflate;
                        compressed_ok = true;
                    }
                }
            }
#endif

            if (!compressed_ok)
            {
                staged.compressedData.clear();
                staged.compression = PakCompression::Stored;
            }
        }
        else
        {
            staged.compression = PakCompression::Stored;
        }

        m_files.push_back(std::move(staged));
    }

    // =========================================================================
    // AddDirectory
    // =========================================================================

    void SparkPakWriter::AddDirectory(const std::filesystem::path& rootPath, const std::string& virtualPrefix)
    {
        if (!std::filesystem::exists(rootPath) || !std::filesystem::is_directory(rootPath))
            return;

        std::error_code pathEc;
        const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(rootPath, pathEc);
        if (pathEc)
            return;

        for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(rootPath))
        {
            // Do not follow file symlinks while packaging. In addition, verify
            // the resolved path remains under the resolved root so junctions or
            // other platform-specific indirections cannot escape the source tree.
            pathEc.clear();
            const auto entryStatus = dirEntry.symlink_status(pathEc);
            if (pathEc || std::filesystem::is_symlink(entryStatus))
                continue;

            if (!dirEntry.is_regular_file(pathEc) || pathEc)
                continue;

            const std::filesystem::path canonicalEntry = std::filesystem::weakly_canonical(dirEntry.path(), pathEc);
            if (pathEc)
                continue;

            const std::filesystem::path relative = canonicalEntry.lexically_relative(canonicalRoot);
            if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "SparkPakWriter: rejected path outside packaging root '%s'",
                               dirEntry.path().string().c_str());
                continue;
            }

            auto relativePath = relative.generic_string();
            auto virtualPath = virtualPrefix.empty() ? relativePath : virtualPrefix + relativePath;

            // Read file contents
            std::ifstream ifs(canonicalEntry, std::ios::binary);
            if (!ifs)
                continue;

            auto fileSize = dirEntry.file_size();
            std::vector<uint8_t> data(fileSize);
            ifs.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(fileSize));

            AddFile(virtualPath, data);
        }
    }

    // =========================================================================
    // Finalize
    // =========================================================================

    bool SparkPakWriter::Finalize(const std::string& outputPath) const
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "SparkPakWriter::Finalize — writing %zu files to '%s'", m_files.size(),
                       outputPath.c_str());
#ifdef _WIN32
        FILE* file = _wfopen(std::filesystem::path(outputPath).wstring().c_str(), L"wb");
#else
        FILE* file = std::fopen(outputPath.c_str(), "wb");
#endif
        if (!file)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakWriter: failed to create '%s'", outputPath.c_str());
            return false;
        }

        // Helper: write with error checking
        auto writeBytes = [&](const void* data, size_t size) -> bool
        { return std::fwrite(data, 1, size, file) == size; };

        // Write placeholder header (will rewrite at end)
        PakHeader header;
        header.fileCount = static_cast<uint32_t>(m_files.size());
        if (!writeBytes(&header, sizeof(PakHeader)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakWriter: failed to write header to '%s'",
                            outputPath.c_str());
            std::fclose(file);
            return false;
        }

        // Write file data blobs, tracking offsets
        struct WrittenEntry
        {
            uint64_t pathHash;
            uint64_t dataOffset;
            uint32_t compressedSize;
            uint32_t originalSize;
            PakCompression compression;
            std::string virtualPath;
        };

        std::vector<WrittenEntry> entries;
        entries.reserve(m_files.size());

        for (const auto& staged : m_files)
        {
            WrittenEntry we;
            we.pathHash = PakFNV1a(staged.virtualPath);
            we.dataOffset = PAK_FTELL(file);
            we.originalSize = static_cast<uint32_t>(staged.originalData.size());
            we.compression = staged.compression;
            we.virtualPath = staged.virtualPath;

            if (staged.compression == PakCompression::Deflate && !staged.compressedData.empty())
            {
                we.compressedSize = static_cast<uint32_t>(staged.compressedData.size());
                if (!writeBytes(staged.compressedData.data(), staged.compressedData.size()))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakWriter: write failed for '%s'",
                                    staged.virtualPath.c_str());
                    std::fclose(file);
                    return false;
                }
            }
            else
            {
                we.compressedSize = static_cast<uint32_t>(staged.originalData.size());
                if (!writeBytes(staged.originalData.data(), staged.originalData.size()))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakWriter: write failed for '%s'",
                                    staged.virtualPath.c_str());
                    std::fclose(file);
                    return false;
                }
            }

            entries.push_back(std::move(we));
        }

        // Build raw TOC buffer
        std::vector<uint8_t> tocRaw;
        for (const auto& we : entries)
        {
            auto pathLen = static_cast<uint16_t>(we.virtualPath.size());
            size_t entrySize = 8 + 8 + 4 + 4 + 1 + 2 + pathLen;
            size_t offset = tocRaw.size();
            tocRaw.resize(offset + entrySize);
            uint8_t* ptr = tocRaw.data() + offset;

            std::memcpy(ptr, &we.pathHash, 8);
            ptr += 8;
            std::memcpy(ptr, &we.dataOffset, 8);
            ptr += 8;
            std::memcpy(ptr, &we.compressedSize, 4);
            ptr += 4;
            std::memcpy(ptr, &we.originalSize, 4);
            ptr += 4;
            *ptr = static_cast<uint8_t>(we.compression);
            ptr += 1;
            std::memcpy(ptr, &pathLen, 2);
            ptr += 2;
            std::memcpy(ptr, we.virtualPath.data(), pathLen);
        }

        // Compress TOC
        header.tocOffset = PAK_FTELL(file);
        header.tocRawSize = static_cast<uint32_t>(tocRaw.size());

#ifdef SPARK_MINIZ_AVAILABLE
        mz_ulong compBound = mz_compressBound(static_cast<mz_ulong>(tocRaw.size()));
        std::vector<uint8_t> tocCompressed(compBound);
        mz_ulong destLen = compBound;

        if (mz_compress(tocCompressed.data(), &destLen, tocRaw.data(), static_cast<mz_ulong>(tocRaw.size())) == MZ_OK)
        {
            tocCompressed.resize(destLen);
            header.tocSize = static_cast<uint32_t>(destLen);
            if (!writeBytes(tocCompressed.data(), destLen))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakWriter: failed to write TOC to '%s'",
                                outputPath.c_str());
                std::fclose(file);
                return false;
            }
        }
        else
#endif
        {
            // Fallback: store TOC uncompressed
            header.tocSize = header.tocRawSize;
            if (!writeBytes(tocRaw.data(), tocRaw.size()))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakWriter: failed to write TOC to '%s'",
                                outputPath.c_str());
                std::fclose(file);
                return false;
            }
        }

        // Rewrite header with final values
        PAK_FSEEK(file, 0, SEEK_SET);
        if (!writeBytes(&header, sizeof(PakHeader)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakWriter: failed to rewrite header in '%s'",
                            outputPath.c_str());
            std::fclose(file);
            return false;
        }

        std::fclose(file);
        return true;
    }

    uint32_t SparkPakWriter::GetFileCount() const
    {
        return static_cast<uint32_t>(m_files.size());
    }

} // namespace Spark
