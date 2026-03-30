/**
 * @file ArchiveResourceProvider.h
 * @brief IResourceProvider backed by a SparkPak (.spk) archive
 *
 * Bridges SparkPakReader into the VirtualFileSystem, allowing .spk
 * archives to be mounted alongside loose file directories with
 * priority-based layering.
 *
 * ## Usage
 * @code
 *   auto& vfs = VirtualFileSystem::GetInstance();
 *   vfs.Mount("base", std::make_unique<ArchiveResourceProvider>("Data/base.spk"), ENGINE_PRIORITY);
 * @endcode
 *
 * @see VirtualFileSystem.h
 * @see SparkPak.h
 */

#pragma once

#include "VirtualFileSystem.h"

#include "Core/SparkPak.h"
#include <memory>
#include <string>

namespace Spark
{

    /**
     * @brief VFS resource provider that reads from a SparkPak archive.
     *
     * Takes ownership of a SparkPakReader. All reads are forwarded
     * directly to the reader's O(1) hash lookup.
     */
    class ArchiveResourceProvider : public IResourceProvider
    {
      public:
        /// @brief Construct from a .spk file path. Opens the archive immediately.
        explicit ArchiveResourceProvider(const std::string& archivePath);

        /// @brief Construct from an already-opened reader (takes ownership).
        explicit ArchiveResourceProvider(std::unique_ptr<SparkPakReader> reader);

        /// @brief Whether the archive was opened successfully.
        bool IsValid() const;

        // IResourceProvider interface
        bool Exists(const std::string& virtualPath) const override;
        std::vector<uint8_t> ReadFile(const std::string& virtualPath) const override;
        std::string ReadTextFile(const std::string& virtualPath) const override;
        std::vector<std::string> ListFiles(const std::string& directory,
                                           const std::string& extension = "") const override;
        std::string GetProviderName() const override;

      private:
        std::unique_ptr<SparkPakReader> m_reader;
    };

} // namespace Spark
