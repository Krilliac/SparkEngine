/**
 * @file SparkPak.cpp
 * @brief SparkPakReader implementation — opens .spk archives and reads entries
 */

#include "SparkPak.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

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
    // Move semantics
    // =========================================================================

    SparkPakReader::~SparkPakReader()
    {
        Close();
    }

    SparkPakReader::SparkPakReader(SparkPakReader&& other) noexcept
        : m_filePath(std::move(other.m_filePath)), m_file(other.m_file), m_header(other.m_header),
          m_entries(std::move(other.m_entries)), m_entryList(std::move(other.m_entryList))
    {
        other.m_file = nullptr;
    }

    SparkPakReader& SparkPakReader::operator=(SparkPakReader&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_filePath = std::move(other.m_filePath);
            m_file = other.m_file;
            m_header = other.m_header;
            m_entries = std::move(other.m_entries);
            m_entryList = std::move(other.m_entryList);
            other.m_file = nullptr;
        }
        return *this;
    }

    // =========================================================================
    // Open / Close
    // =========================================================================

    bool SparkPakReader::Open(const std::string& filePath)
    {
        Close();

        m_filePath = filePath;
#ifdef _WIN32
        m_file = _wfopen(std::filesystem::path(filePath).wstring().c_str(), L"rb");
#else
        m_file = std::fopen(filePath.c_str(), "rb");
#endif
        if (!m_file)
            return false;

        if (!ReadHeader() || !ReadTOC())
        {
            Close();
            return false;
        }

        return true;
    }

    void SparkPakReader::Close()
    {
        if (m_file)
        {
            std::fclose(m_file);
            m_file = nullptr;
        }
        m_entries.clear();
        m_entryList.clear();
        m_header = {};
    }

    bool SparkPakReader::IsOpen() const
    {
        return m_file != nullptr;
    }

    // =========================================================================
    // Header / TOC parsing
    // =========================================================================

    bool SparkPakReader::ReadHeader()
    {
        if (std::fread(&m_header, sizeof(PakHeader), 1, m_file) != 1)
            return false;

        return m_header.magic == kSparkPakMagic && m_header.version == kSparkPakVersion;
    }

    bool SparkPakReader::ReadTOC()
    {
#ifndef SPARK_MINIZ_AVAILABLE
        // Without miniz we can only read uncompressed TOCs
        if (m_header.tocSize != m_header.tocRawSize)
            return false;
#endif

        // Seek to TOC
        if (PAK_FSEEK(m_file, m_header.tocOffset, SEEK_SET) != 0)
            return false;

        // Read compressed TOC blob
        std::vector<uint8_t> tocCompressed(m_header.tocSize);
        if (std::fread(tocCompressed.data(), 1, m_header.tocSize, m_file) != m_header.tocSize)
            return false;

        // Decompress TOC
        std::vector<uint8_t> tocRaw;
        if (m_header.tocSize == m_header.tocRawSize)
        {
            // TOC stored uncompressed
            tocRaw = std::move(tocCompressed);
        }
        else
        {
#ifdef SPARK_MINIZ_AVAILABLE
            tocRaw.resize(m_header.tocRawSize);
            mz_ulong destLen = m_header.tocRawSize;
            if (mz_uncompress(tocRaw.data(), &destLen, tocCompressed.data(), m_header.tocSize) != MZ_OK)
                return false;
#else
            return false;
#endif
        }

        // Parse TOC entries from raw buffer
        const uint8_t* ptr = tocRaw.data();
        const uint8_t* end = ptr + tocRaw.size();

        m_entries.reserve(m_header.fileCount);
        for (uint32_t i = 0; i < m_header.fileCount; ++i)
        {
            // Need at least: pathHash(8) + dataOffset(8) + compressedSize(4) + originalSize(4) + compression(1) +
            // pathLength(2) = 27 bytes
            if (ptr + 27 > end)
                return false;

            PakEntry entry;
            std::memcpy(&entry.pathHash, ptr, 8);
            ptr += 8;
            std::memcpy(&entry.dataOffset, ptr, 8);
            ptr += 8;
            std::memcpy(&entry.compressedSize, ptr, 4);
            ptr += 4;
            std::memcpy(&entry.originalSize, ptr, 4);
            ptr += 4;
            entry.compression = static_cast<PakCompression>(*ptr);
            ptr += 1;

            uint16_t pathLen = 0;
            std::memcpy(&pathLen, ptr, 2);
            ptr += 2;

            if (ptr + pathLen > end)
                return false;

            entry.virtualPath.assign(reinterpret_cast<const char*>(ptr), pathLen);
            ptr += pathLen;

            auto hash = entry.pathHash;
            m_entries.emplace(hash, std::move(entry));
        }

        // Build iteration list
        m_entryList.clear();
        m_entryList.reserve(m_entries.size());
        for (auto& [hash, entry] : m_entries)
            m_entryList.push_back(&entry);

        return true;
    }

    // =========================================================================
    // Query
    // =========================================================================

    bool SparkPakReader::Exists(const std::string& virtualPath) const
    {
        return m_entries.contains(PakFNV1a(virtualPath));
    }

    std::vector<uint8_t> SparkPakReader::ReadFile(const std::string& virtualPath) const
    {
        auto it = m_entries.find(PakFNV1a(virtualPath));
        if (it == m_entries.end())
            return {};

        const auto& entry = it->second;

        // Seek and read compressed data
        if (PAK_FSEEK(m_file, entry.dataOffset, SEEK_SET) != 0)
            return {};

        std::vector<uint8_t> compressed(entry.compressedSize);
        if (std::fread(compressed.data(), 1, entry.compressedSize, m_file) != entry.compressedSize)
            return {};

        if (entry.compression == PakCompression::Stored)
            return compressed;

#ifdef SPARK_MINIZ_AVAILABLE
        std::vector<uint8_t> decompressed(entry.originalSize);
        mz_ulong destLen = entry.originalSize;
        if (mz_uncompress(decompressed.data(), &destLen, compressed.data(), entry.compressedSize) != MZ_OK)
            return {};
        return decompressed;
#else
        return {};
#endif
    }

    std::string SparkPakReader::ReadTextFile(const std::string& virtualPath) const
    {
        auto data = ReadFile(virtualPath);
        return std::string(data.begin(), data.end());
    }

    std::vector<std::string> SparkPakReader::ListFiles(const std::string& directory, const std::string& extension) const
    {
        std::vector<std::string> result;
        for (const auto* entry : m_entryList)
        {
            if (!directory.empty() && entry->virtualPath.find(directory) != 0)
                continue;

            if (!extension.empty())
            {
                auto dotPos = entry->virtualPath.rfind('.');
                if (dotPos == std::string::npos)
                    continue;
                auto ext = entry->virtualPath.substr(dotPos);
                if (ext != extension)
                    continue;
            }

            result.push_back(entry->virtualPath);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    uint32_t SparkPakReader::GetEntryCount() const
    {
        return static_cast<uint32_t>(m_entries.size());
    }

    const std::string& SparkPakReader::GetFilePath() const
    {
        return m_filePath;
    }

} // namespace Spark
