/**
 * @file ArchiveResourceProvider.cpp
 * @brief IResourceProvider implementation backed by SparkPak (.spk) archives
 */

#include "ArchiveResourceProvider.h"

#include <filesystem>

namespace Spark
{

    ArchiveResourceProvider::ArchiveResourceProvider(const std::string& archivePath)
        : m_reader(std::make_unique<SparkPakReader>())
    {
        if (!m_reader->Open(archivePath))
            m_reader.reset();
    }

    ArchiveResourceProvider::ArchiveResourceProvider(std::unique_ptr<SparkPakReader> reader)
        : m_reader(std::move(reader))
    {
    }

    bool ArchiveResourceProvider::IsValid() const
    {
        return m_reader && m_reader->IsOpen();
    }

    bool ArchiveResourceProvider::Exists(const std::string& virtualPath) const
    {
        return m_reader && m_reader->Exists(virtualPath);
    }

    std::vector<uint8_t> ArchiveResourceProvider::ReadFile(const std::string& virtualPath) const
    {
        if (!m_reader)
            return {};
        return m_reader->ReadFile(virtualPath);
    }

    std::string ArchiveResourceProvider::ReadTextFile(const std::string& virtualPath) const
    {
        if (!m_reader)
            return {};
        return m_reader->ReadTextFile(virtualPath);
    }

    std::vector<std::string> ArchiveResourceProvider::ListFiles(const std::string& directory,
                                                                const std::string& extension) const
    {
        if (!m_reader)
            return {};
        return m_reader->ListFiles(directory, extension);
    }

    std::string ArchiveResourceProvider::GetProviderName() const
    {
        if (!m_reader)
            return "ArchiveResourceProvider [invalid]";
        return "SparkPak:" + std::filesystem::path(m_reader->GetFilePath()).filename().string();
    }

} // namespace Spark
