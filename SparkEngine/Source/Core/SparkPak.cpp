/**
 * @file SparkPak.cpp
 * @brief SparkPakReader implementation — opens .spk archives and reads entries
 */

#include "SparkPak.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>

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

    namespace
    {
        constexpr uint64_t kMaxTocBytes = 256ull * 1024ull * 1024ull;   // 256 MB
        constexpr uint32_t kMaxFileCount = 10'000'000u;                 // 10M entries
        constexpr uint32_t kMaxEntryBytes = 2u * 1024u * 1024u * 1024u; // 2 GB

        // A .spk is untrusted input (every archive found in ./Data is mounted at
        // startup). kMaxEntryBytes alone lets a 100-byte entry declare a 2 GB
        // originalSize, and ReadFile allocates that buffer BEFORE decompression can
        // fail. The bound is therefore a per-entry decompression budget, checked in
        // ReadFile: an entry that violates it fails on its own instead of unmounting
        // the archive, because legitimate cooked content does reach extreme ratios
        // (a zero-filled lightmap or padded heightmap deflates at deflate's ~1032:1
        // ceiling, and zstd goes far past it) and an archive-wide rejection at Open
        // would take every other asset in the pak down with it.
        //
        // Two bounds, both per entry:
        //  - an absolute ceiling on the buffer any single entry can demand, and
        //  - an expansion factor above every codec's maximum, so a tiny compressed
        //    blob still cannot claim a huge output.
        // The output itself is bounded at decompression time as well: the destination
        // buffer is exactly originalSize and both mz_uncompress and ZSTD_decompress
        // are told that size, so neither can stream more bytes than the budget allows.
        constexpr uint64_t kMaxDecompressedEntryBytes = 256ull * 1024ull * 1024ull; // 256 MB
        constexpr uint64_t kMaxCompressionRatio = 100'000ull;

        /// Reject an entry whose declared decompressed size is outside the per-entry
        /// budget. Returns false (and logs) for the offending entry only.
        bool WithinDecompressionBudget(const PakEntry& entry, const std::string& virtualPath)
        {
            if (entry.compression == PakCompression::Stored)
                return true;

            if (static_cast<uint64_t>(entry.originalSize) > kMaxDecompressedEntryBytes)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                "SparkPak: '%s' declares %u decompressed bytes, over the %llu byte per-entry budget",
                                virtualPath.c_str(), entry.originalSize,
                                static_cast<unsigned long long>(kMaxDecompressedEntryBytes));
                return false;
            }

            if (entry.compressedSize == 0 && entry.originalSize != 0)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPak: '%s' declares %u bytes from an empty payload",
                                virtualPath.c_str(), entry.originalSize);
                return false;
            }

            if (static_cast<uint64_t>(entry.originalSize) >
                static_cast<uint64_t>(entry.compressedSize) * kMaxCompressionRatio)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                "SparkPak: '%s' declares a %u:%u expansion, over the %llu:1 cap", virtualPath.c_str(),
                                entry.originalSize, entry.compressedSize,
                                static_cast<unsigned long long>(kMaxCompressionRatio));
                return false;
            }

            return true;
        }

#if defined(SPARK_MINIZ_AVAILABLE) || defined(SPARK_ZSTD_AVAILABLE)
        /// Allocate an entry's decompressed buffer, turning a hostile or merely
        /// huge originalSize into a clean failure instead of a std::bad_alloc
        /// thrown out of an asset-loading path that has no handler.
        bool AllocateDecompressBuffer(std::vector<uint8_t>& buffer, const PakEntry& entry,
                                      const std::string& virtualPath)
        {
            try
            {
                buffer.resize(entry.originalSize);
            }
            catch (const std::bad_alloc&)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core,
                                "SparkPak: out of memory decompressing '%s' (%u bytes declared)", virtualPath.c_str(),
                                entry.originalSize);
                return false;
            }
            return true;
        }
#endif
    } // namespace

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

        SPARK_LOG_INFO(Spark::LogCategory::Core, "SparkPakReader::Open — '%s'", filePath.c_str());
        m_filePath = filePath;
#ifdef _WIN32
        m_file = _wfopen(std::filesystem::path(filePath).wstring().c_str(), L"rb");
#else
        m_file = std::fopen(filePath.c_str(), "rb");
#endif
        if (!m_file)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakReader: failed to open '%s'", filePath.c_str());
            return false;
        }

        if (!ReadHeader() || !ReadTOC())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPakReader: invalid header/TOC in '%s'", filePath.c_str());
            Close();
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "SparkPakReader: opened '%s' — %zu entries", filePath.c_str(),
                       m_entryList.size());
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

        // Sanity-check header sizes against physical file size to avoid std::bad_alloc
        // on a corrupted or truncated archive (tocSize/tocRawSize/fileCount are all
        // untrusted inputs from disk).
        if (PAK_FSEEK(m_file, 0, SEEK_END) != 0)
            return false;
        const uint64_t fileSize = PAK_FTELL(m_file);

        // Hard upper bounds to reject pathological headers early.
        if (m_header.tocSize == 0 || m_header.tocSize > kMaxTocBytes)
            return false;
        if (m_header.tocRawSize == 0 || m_header.tocRawSize > kMaxTocBytes)
            return false;
        if (m_header.fileCount > kMaxFileCount)
            return false;
        if (m_header.tocOffset < sizeof(PakHeader) || m_header.tocOffset >= fileSize)
            return false;
        if (m_header.tocSize > fileSize - m_header.tocOffset)
            return false;

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

        // Reserve conservatively — use the smaller of the declared count and the
        // raw TOC size divided by the minimum per-entry size (27 bytes). This keeps
        // a corrupted-but-bounded fileCount from reserving an absurdly large hash map.
        constexpr size_t kMinEntryBytes = 27;
        const size_t maxPossibleEntries = tocRaw.size() / kMinEntryBytes;
        m_entries.reserve(std::min<size_t>(m_header.fileCount, maxPossibleEntries));
        for (uint32_t i = 0; i < m_header.fileCount; ++i)
        {
            // Need at least: pathHash(8) + dataOffset(8) + compressedSize(4) + originalSize(4) + compression(1) +
            // pathLength(2) = 27 bytes
            if (static_cast<size_t>(end - ptr) < 27)
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

            if (static_cast<size_t>(end - ptr) < pathLen)
                return false;

            entry.virtualPath.assign(reinterpret_cast<const char*>(ptr), pathLen);
            ptr += pathLen;

            // Validate every untrusted entry while opening the archive, before a
            // later lookup can allocate from its declared sizes. File data is
            // written before the TOC, so the subtraction form below both rejects
            // TOC overlap/truncation and avoids integer overflow.
            if (entry.compression != PakCompression::Stored && entry.compression != PakCompression::Deflate &&
                entry.compression != PakCompression::Zstd)
            {
                return false;
            }
            if (entry.compressedSize > kMaxEntryBytes || entry.originalSize > kMaxEntryBytes)
                return false;
            if (entry.dataOffset < sizeof(PakHeader) || entry.dataOffset > m_header.tocOffset)
                return false;
            if (entry.compressedSize > m_header.tocOffset - entry.dataOffset)
                return false;
            if (entry.compression == PakCompression::Stored && entry.compressedSize != entry.originalSize)
                return false;
            // The decompression budget is deliberately NOT enforced here: it is a
            // per-entry refusal in ReadFile, so one over-compressible or hostile entry
            // cannot fail Open() and unmount every other asset in the archive.

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

        // Refuse this entry before touching the file if its declared expansion is
        // outside the per-entry budget. Other entries in the archive stay readable.
        if (!WithinDecompressionBudget(entry, virtualPath))
            return {};

        // The declared sizes are attacker-controlled. ReadTOC bounds them, but the
        // allocations here still fail hard on a memory-starved machine, and a
        // throwing vector constructor would propagate out of an asset-loading path
        // that has no handler.
        std::vector<uint8_t> compressed;
        try
        {
            compressed.resize(entry.compressedSize);
        }
        catch (const std::bad_alloc&)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "SparkPak: out of memory reading '%s' (%u compressed bytes)",
                            virtualPath.c_str(), entry.compressedSize);
            return {};
        }
        {
            // A FILE* has one shared cursor. Keep seek+read indivisible so two
            // concurrent asset loads cannot redirect each other's reads.
            std::lock_guard<std::mutex> lock(m_fileMutex);
            if (PAK_FSEEK(m_file, entry.dataOffset, SEEK_SET) != 0)
                return {};
            if (std::fread(compressed.data(), 1, entry.compressedSize, m_file) != entry.compressedSize)
                return {};
        }

        if (entry.compression == PakCompression::Stored)
            return compressed;

        if (entry.compression == PakCompression::Deflate)
        {
#ifdef SPARK_MINIZ_AVAILABLE
            std::vector<uint8_t> decompressed;
            if (!AllocateDecompressBuffer(decompressed, entry, virtualPath))
                return {};
            mz_ulong destLen = entry.originalSize;
            if (mz_uncompress(decompressed.data(), &destLen, compressed.data(), entry.compressedSize) != MZ_OK)
                return {};
            if (destLen != entry.originalSize)
                return {};
            return decompressed;
#else
            return {};
#endif
        }

        if (entry.compression == PakCompression::Zstd)
        {
#ifdef SPARK_ZSTD_AVAILABLE
            std::vector<uint8_t> decompressed;
            if (!AllocateDecompressBuffer(decompressed, entry, virtualPath))
                return {};
            size_t result =
                ZSTD_decompress(decompressed.data(), entry.originalSize, compressed.data(), entry.compressedSize);
            if (ZSTD_isError(result) || result != entry.originalSize)
                return {};
            return decompressed;
#else
            return {};
#endif
        }

        return {};
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
