/**
 * @file SparkPakWriter.h
 * @brief Builder for SparkPak (.spk) archive files
 *
 * ## Usage
 * @code
 *   SparkPakWriter writer;
 *   writer.AddFile("textures/brick.png", brickData);
 *   writer.AddDirectory("Assets/", "");  // recursively add all files
 *   writer.Finalize("Data/base.spk");
 * @endcode
 *
 * @see SparkPak.h for the archive format and reader
 */

#pragma once

#include "SparkPak.h"
#include <filesystem>
#include <string>
#include <vector>

namespace Spark
{

    /**
     * @brief Builds .spk archive files from in-memory data or filesystem directories.
     *
     * Files are compressed with deflate by default. Compression is skipped when the
     * compressed output is >= 95% of the original size (not worth the CPU cost).
     */
    class SparkPakWriter
    {
      public:
        SparkPakWriter() = default;

        /// @brief Add a file from a raw byte buffer.
        /// @param virtualPath  Path inside the archive (e.g. "textures/brick.png").
        /// @param data         File contents.
        /// @param compress     Whether to attempt deflate compression.
        void AddFile(const std::string& virtualPath, const std::vector<uint8_t>& data, bool compress = true);

        /// @brief Recursively add all files from a directory.
        /// @param rootPath       Filesystem directory to scan.
        /// @param virtualPrefix  Prefix prepended to virtual paths (e.g. "" or "assets/").
        void AddDirectory(const std::filesystem::path& rootPath, const std::string& virtualPrefix = "");

        /// @brief Write the archive to disk.
        /// @param outputPath  Destination file path.
        /// @return true on success.
        bool Finalize(const std::string& outputPath) const;

        /// @brief Number of files staged for writing.
        uint32_t GetFileCount() const;

      private:
        struct StagedFile
        {
            std::string virtualPath;
            std::vector<uint8_t> originalData;
            std::vector<uint8_t> compressedData;
            PakCompression compression = PakCompression::Stored;
        };

        std::vector<StagedFile> m_files;
    };

} // namespace Spark
