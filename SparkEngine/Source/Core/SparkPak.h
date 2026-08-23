/**
 * @file SparkPak.h
 * @brief MPQ-inspired binary archive format for asset packaging
 *
 * SparkPak (.spk) archives bundle multiple files into a single binary
 * container with per-file deflate compression and O(1) hash-based lookup.
 *
 * ## File Format
 * ```
 * [Header 32 bytes] [File data blobs...] [Compressed TOC at end]
 * ```
 *
 * The table of contents sits at the end of the file (like ZIP's central
 * directory), enabling streaming writes and easy appending.
 *
 * ## Usage
 * @code
 *   SparkPakReader reader;
 *   if (reader.Open("Data/base.spk"))
 *   {
 *       auto data = reader.ReadFile("textures/brick.png");
 *       auto listing = reader.ListFiles("textures/", ".png");
 *   }
 * @endcode
 *
 * @see SparkPakWriter.h for archive creation
 * @see ArchiveResourceProvider.h for VFS integration
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Spark
{

    // =========================================================================
    // Format constants
    // =========================================================================

    constexpr uint32_t kSparkPakMagic = 0x314B5053; // "SPK1" little-endian
    constexpr uint32_t kSparkPakVersion = 1;

    /// @brief Compression method for individual entries.
    enum class PakCompression : uint8_t
    {
        Stored = 0,  ///< No compression
        Deflate = 1, ///< zlib/deflate via miniz
        Zstd = 2,    ///< Zstandard compression (faster decompression, better ratios)
    };

    // =========================================================================
    // On-disk structures
    // =========================================================================

    /// @brief 32-byte file header at offset 0.
#pragma pack(push, 1)
    struct PakHeader
    {
        uint32_t magic = kSparkPakMagic;
        uint32_t version = kSparkPakVersion;
        uint32_t fileCount = 0;
        uint32_t reserved1 = 0;
        uint64_t tocOffset = 0;  ///< Byte offset of compressed TOC
        uint32_t tocSize = 0;    ///< Compressed size of TOC blob
        uint32_t tocRawSize = 0; ///< Uncompressed size of TOC blob
    };
#pragma pack(pop)

    static_assert(sizeof(PakHeader) == 32, "PakHeader must be 32 bytes");

    /// @brief In-memory TOC entry (deserialized from archive).
    struct PakEntry
    {
        uint64_t pathHash = 0;
        uint64_t dataOffset = 0;
        uint32_t compressedSize = 0;
        uint32_t originalSize = 0;
        PakCompression compression = PakCompression::Stored;
        std::string virtualPath;
    };

    // =========================================================================
    // FNV-1a hash (matches AssetHandle)
    // =========================================================================

    /// @brief 64-bit FNV-1a hash matching AssetHandle::FromPath.
    constexpr uint64_t PakFNV1a(std::string_view str)
    {
        constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
        constexpr uint64_t kPrime = 1099511628211ULL;
        uint64_t h = kOffsetBasis;
        for (char c : str)
        {
            h ^= static_cast<uint64_t>(c);
            h *= kPrime;
        }
        return h;
    }

    // =========================================================================
    // SparkPakReader
    // =========================================================================

    /**
     * @brief Read-only accessor for .spk archive files.
     *
     * Opens an archive, reads the TOC, and provides O(1) lookup by
     * virtual path. Thread-safe for concurrent reads after Open().
     */
    class SparkPakReader
    {
      public:
        SparkPakReader() = default;
        ~SparkPakReader();

        SparkPakReader(const SparkPakReader&) = delete;
        SparkPakReader& operator=(const SparkPakReader&) = delete;
        SparkPakReader(SparkPakReader&& other) noexcept;
        SparkPakReader& operator=(SparkPakReader&& other) noexcept;

        /// @brief Open an archive file and read its TOC.
        /// @return true on success.
        bool Open(const std::string& filePath);

        /// @brief Close the archive and release resources.
        void Close();

        /// @brief Check if the archive is open and valid.
        bool IsOpen() const;

        /// @brief Check whether a virtual path exists in this archive.
        bool Exists(const std::string& virtualPath) const;

        /// @brief Read a file's contents as a binary blob.
        std::vector<uint8_t> ReadFile(const std::string& virtualPath) const;

        /// @brief Read a file's contents as a UTF-8 string.
        std::string ReadTextFile(const std::string& virtualPath) const;

        /// @brief List files under a directory prefix, optionally filtered by extension.
        std::vector<std::string> ListFiles(const std::string& directory, const std::string& extension = "") const;

        /// @brief Number of entries in this archive.
        uint32_t GetEntryCount() const;

        /// @brief The file path this archive was opened from.
        const std::string& GetFilePath() const;

      private:
        bool ReadHeader();
        bool ReadTOC();

        std::string m_filePath;
        FILE* m_file = nullptr;
        PakHeader m_header{};
        std::unordered_map<uint64_t, PakEntry> m_entries;
        std::vector<PakEntry*> m_entryList; ///< For iteration (ListFiles)
        /// Serializes the seek+read pair on the shared C file stream.  Individual
        /// stdio calls may lock internally, but a seek followed by a read is not
        /// an atomic operation unless we hold one lock across both calls.
        mutable std::mutex m_fileMutex;
    };

} // namespace Spark
