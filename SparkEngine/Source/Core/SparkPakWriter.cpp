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

#ifdef SPARK_MINIZ_AVAILABLE
        if (compress && !data.empty())
        {
            mz_ulong compBound = mz_compressBound(static_cast<mz_ulong>(data.size()));
            staged.compressedData.resize(compBound);
            mz_ulong destLen = compBound;

            if (mz_compress(staged.compressedData.data(), &destLen, data.data(), static_cast<mz_ulong>(data.size())) ==
                MZ_OK)
            {
                staged.compressedData.resize(destLen);

                // Only use compression if it saves at least 5%
                if (destLen < data.size() * 95 / 100)
                {
                    staged.compression = PakCompression::Deflate;
                }
                else
                {
                    staged.compressedData.clear();
                    staged.compression = PakCompression::Stored;
                }
            }
            else
            {
                staged.compressedData.clear();
                staged.compression = PakCompression::Stored;
            }
        }
        else
#else
        (void)compress;
#endif
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

        for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(rootPath))
        {
            if (!dirEntry.is_regular_file())
                continue;

            auto relativePath = std::filesystem::relative(dirEntry.path(), rootPath).generic_string();
            auto virtualPath = virtualPrefix.empty() ? relativePath : virtualPrefix + relativePath;

            // Read file contents
            std::ifstream ifs(dirEntry.path(), std::ios::binary);
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
