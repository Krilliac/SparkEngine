/**
 * @file VirtualFileSystem.cpp
 * @brief Implementation of the mount-priority virtual filesystem
 */

#include "VirtualFileSystem.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace Spark
{

    // =========================================================================
    // LocalFileProvider
    // =========================================================================

    LocalFileProvider::LocalFileProvider(const std::string& rootPath) : m_rootPath(rootPath)
    {
        // Normalize trailing separator
        if (!m_rootPath.empty() && m_rootPath.back() != '/' && m_rootPath.back() != '\\')
        {
            m_rootPath += '/';
        }
    }

    std::string LocalFileProvider::ResolvePath(const std::string& virtualPath) const
    {
        return m_rootPath + virtualPath;
    }

    bool LocalFileProvider::Exists(const std::string& virtualPath) const
    {
        return fs::exists(ResolvePath(virtualPath));
    }

    std::vector<uint8_t> LocalFileProvider::ReadFile(const std::string& virtualPath) const
    {
        std::string fullPath = ResolvePath(virtualPath);
        std::ifstream file(fullPath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Failed to open file '%s'", fullPath.c_str());
            return {};
        }

        auto size = file.tellg();
        if (size <= 0)
        {
            return {};
        }

        std::vector<uint8_t> buffer(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(buffer.data()), size);
        if (file.bad())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Read error for '%s'", fullPath.c_str());
            return {};
        }
        return buffer;
    }

    std::string LocalFileProvider::ReadTextFile(const std::string& virtualPath) const
    {
        std::string fullPath = ResolvePath(virtualPath);
        std::ifstream file(fullPath);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "VFS: Failed to open text file '%s'", fullPath.c_str());
            return {};
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    std::vector<std::string> LocalFileProvider::ListFiles(const std::string& directory,
                                                          const std::string& extension) const
    {
        std::vector<std::string> results;
        std::string fullDir = ResolvePath(directory);

        if (!fs::exists(fullDir) || !fs::is_directory(fullDir))
        {
            return results;
        }

        for (const auto& entry : fs::directory_iterator(fullDir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            std::string filename = entry.path().filename().string();

            if (!extension.empty())
            {
                std::string ext = entry.path().extension().string();
                if (ext != extension)
                {
                    continue;
                }
            }

            // Return virtual path relative to the mount root
            std::string virtualPath = directory;
            if (!virtualPath.empty() && virtualPath.back() != '/')
            {
                virtualPath += '/';
            }
            virtualPath += filename;
            results.push_back(virtualPath);
        }

        return results;
    }

    std::string LocalFileProvider::GetProviderName() const
    {
        return "LocalFile(" + m_rootPath + ")";
    }

    // =========================================================================
    // VirtualFileSystem
    // =========================================================================

    VirtualFileSystem& VirtualFileSystem::GetInstance()
    {
        static VirtualFileSystem instance;
        return instance;
    }

    bool VirtualFileSystem::Initialize()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized)
        {
            return true;
        }

        m_mounts.clear();
        m_initialized = true;
        return true;
    }

    void VirtualFileSystem::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mounts.clear();
        m_initialized = false;
    }

    void VirtualFileSystem::Mount(const std::string& name, std::unique_ptr<IResourceProvider> provider,
                                  int32_t priority)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Remove existing mount with same name, if any
        std::erase_if(m_mounts, [&name](const MountPoint& mp) { return mp.name == name; });

        MountPoint mp;
        mp.name = name;
        mp.provider = std::move(provider);
        mp.priority = priority;
        m_mounts.push_back(std::move(mp));
    }

    void VirtualFileSystem::Unmount(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::erase_if(m_mounts, [&name](const MountPoint& mp) { return mp.name == name; });
    }

    std::vector<const MountPoint*> VirtualFileSystem::GetSortedMounts() const
    {
        std::vector<const MountPoint*> sorted;
        sorted.reserve(m_mounts.size());
        for (const auto& mp : m_mounts)
        {
            sorted.push_back(&mp);
        }

        // Descending priority — highest priority first
        std::sort(sorted.begin(), sorted.end(),
                  [](const MountPoint* a, const MountPoint* b) { return a->priority > b->priority; });

        return sorted;
    }

    bool VirtualFileSystem::Exists(const std::string& virtualPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto* mp : GetSortedMounts())
        {
            if (mp->provider->Exists(virtualPath))
            {
                return true;
            }
        }
        return false;
    }

    std::vector<uint8_t> VirtualFileSystem::ReadFile(const std::string& virtualPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto sorted = GetSortedMounts();
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            if (sorted[i]->provider->Exists(virtualPath))
            {
                auto data = sorted[i]->provider->ReadFile(virtualPath);
                if (!data.empty())
                    return data;

                // File exists but read failed — try next mount
                if (i + 1 < sorted.size())
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core,
                                   "VFS: '%s' read failed in [%s], falling back to next mount", virtualPath.c_str(),
                                   sorted[i]->name.c_str());
                }
            }
        }
        return {};
    }

    std::string VirtualFileSystem::ReadTextFile(const std::string& virtualPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto sorted = GetSortedMounts();
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            if (sorted[i]->provider->Exists(virtualPath))
            {
                auto text = sorted[i]->provider->ReadTextFile(virtualPath);
                if (!text.empty())
                    return text;

                // File exists but read failed — try next mount
                if (i + 1 < sorted.size())
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Core,
                                   "VFS: '%s' text read failed in [%s], falling back to next mount",
                                   virtualPath.c_str(), sorted[i]->name.c_str());
                }
            }
        }
        return {};
    }

    std::vector<std::string> VirtualFileSystem::ListFiles(const std::string& directory,
                                                          const std::string& extension) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Merge results from all mounts, deduplicating by virtual path.
        // Higher-priority mounts are iterated first but all files are included.
        std::unordered_set<std::string> seen;
        std::vector<std::string> results;

        for (const auto* mp : GetSortedMounts())
        {
            auto files = mp->provider->ListFiles(directory, extension);
            for (auto& file : files)
            {
                if (seen.insert(file).second)
                {
                    results.push_back(std::move(file));
                }
            }
        }

        return results;
    }

    std::string VirtualFileSystem::ResolveProvider(const std::string& virtualPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto* mp : GetSortedMounts())
        {
            if (mp->provider->Exists(virtualPath))
            {
                return mp->name;
            }
        }
        return {};
    }

    uint32_t VirtualFileSystem::GetMountCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint32_t>(m_mounts.size());
    }

    std::string VirtualFileSystem::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::ostringstream ss;
        ss << "VirtualFileSystem: " << m_mounts.size() << " mount(s)\n";

        auto sorted = GetSortedMounts();
        for (const auto* mp : sorted)
        {
            ss << "  [" << mp->priority << "] " << mp->name << " -> " << mp->provider->GetProviderName() << "\n";
        }

        return ss.str();
    }

} // namespace Spark
