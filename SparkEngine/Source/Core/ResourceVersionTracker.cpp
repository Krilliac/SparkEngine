/**
 * @file ResourceVersionTracker.cpp
 * @brief Implementation of the resource version tracker
 */

#include "ResourceVersionTracker.h"
#include "Utils/Validate.h"

#include <format>

namespace Spark
{

    ResourceVersionTracker& ResourceVersionTracker::GetInstance()
    {
        static ResourceVersionTracker instance;
        return instance;
    }

    uint32_t ResourceVersionTracker::RegisterResource(const std::string& path)
    {
        auto [it, inserted] = m_versions.try_emplace(path, 1);
        return it->second;
    }

    uint32_t ResourceVersionTracker::GetVersion(const std::string& path) const
    {
        auto it = m_versions.find(path);
        if (it == m_versions.end())
        {
            return 0;
        }
        return it->second;
    }

    uint32_t ResourceVersionTracker::IncrementVersion(const std::string& path)
    {
        auto it = m_versions.find(path);
        if (it == m_versions.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "IncrementVersion: resource '%s' not registered", path.c_str());
            return 0;
        }
        return ++(it->second);
    }

    bool ResourceVersionTracker::IsStale(const std::string& path, uint32_t cachedVersion) const
    {
        auto it = m_versions.find(path);
        if (it == m_versions.end())
        {
            return false;
        }
        return it->second > cachedVersion;
    }

    void ResourceVersionTracker::UnregisterResource(const std::string& path)
    {
        m_versions.erase(path);
    }

    uint32_t ResourceVersionTracker::GetTrackedResourceCount() const
    {
        return static_cast<uint32_t>(m_versions.size());
    }

    std::string ResourceVersionTracker::Console_GetStatus() const
    {
        std::string result = std::format("ResourceVersionTracker: {} resource(s) tracked\n", m_versions.size());

        for (const auto& [path, version] : m_versions)
        {
            result += std::format("  {} -> v{}\n", path, version);
        }

        return result;
    }

    void ResourceVersionTracker::Shutdown()
    {
        m_versions.clear();
    }

} // namespace Spark
