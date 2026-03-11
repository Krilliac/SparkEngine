/**
 * @file ContentDelivery.cpp
 * @brief Implementation of the content delivery and asset bundle system
 */

#include "ContentDelivery.h"

#include <algorithm>
#include <sstream>

namespace Spark
{

    ContentDelivery::ContentDelivery() = default;

    bool ContentDelivery::CheckForUpdates(const std::string& manifestUrl)
    {
        // In a real implementation, this would HTTP GET the manifest URL
        // and parse the JSON response. For now, provide the framework.
        (void)manifestUrl;
        return false;
    }

    std::vector<AssetBundle> ContentDelivery::GetAvailableUpdates() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<AssetBundle> updates;
        for (const auto& bundle : m_remoteManifest.bundles)
        {
            auto localIt = m_localBundles.find(bundle.name);
            if (localIt == m_localBundles.end() || localIt->second.version != bundle.version)
            {
                updates.push_back(bundle);
            }
        }
        return updates;
    }

    bool ContentDelivery::QueueDownload(const std::string& bundleName, int priority)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        DownloadTask task;
        task.bundleName = bundleName;
        task.priority = priority;

        // Find bundle info
        for (const auto& bundle : m_remoteManifest.bundles)
        {
            if (bundle.name == bundleName)
            {
                task.url = bundle.url;
                task.totalBytes = bundle.sizeBytes;
                break;
            }
        }

        m_downloadQueue.push_back(task);

        // Sort by priority (highest first)
        std::sort(m_downloadQueue.begin(), m_downloadQueue.end(),
                  [](const DownloadTask& a, const DownloadTask& b) { return a.priority > b.priority; });

        return true;
    }

    void ContentDelivery::Update(float deltaTime)
    {
        (void)deltaTime;
        // Process download queue — real implementation would use async HTTP
        // For framework purposes, mark tasks as progressing
    }

    float ContentDelivery::GetOverallProgress() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_downloadQueue.empty())
        {
            return 1.0f;
        }
        float total = 0.0f;
        for (const auto& task : m_downloadQueue)
        {
            total += task.progress;
        }
        return total / static_cast<float>(m_downloadQueue.size());
    }

    std::vector<DownloadTask> ContentDelivery::GetDownloadQueue() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_downloadQueue;
    }

    void ContentDelivery::CancelAll()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_downloadQueue.clear();
    }

    bool ContentDelivery::VerifyBundle(const std::string& bundleName) const
    {
        auto it = m_localBundles.find(bundleName);
        if (it == m_localBundles.end())
        {
            return false;
        }
        // Real implementation would compute checksum and compare
        return it->second.installed;
    }

    void ContentDelivery::DeleteBundle(const std::string& bundleName)
    {
        m_localBundles.erase(bundleName);
    }

    void ContentDelivery::OnProgress(std::function<void(const std::string&, float)> callback)
    {
        m_progressCallbacks.push_back(std::move(callback));
    }

    void ContentDelivery::OnComplete(std::function<void(const std::string&, bool)> callback)
    {
        m_completeCallbacks.push_back(std::move(callback));
    }

    std::string ContentDelivery::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;
        oss << "=== Content Delivery ===\n";
        oss << "Install path: " << m_installPath << "\n";
        oss << "Local bundles: " << m_localBundles.size() << "\n";
        oss << "Download queue: " << m_downloadQueue.size() << "\n";
        oss << "Overall progress: " << static_cast<int>(GetOverallProgress() * 100) << "%\n";
        for (const auto& task : m_downloadQueue)
        {
            oss << "  " << task.bundleName << ": " << static_cast<int>(task.progress * 100) << "%"
                << (task.completed ? " [DONE]" : "") << (task.failed ? " [FAILED]" : "") << "\n";
        }
        return oss.str();
    }

} // namespace Spark
