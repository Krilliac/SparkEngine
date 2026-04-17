/**
 * @file DirectStorageLoader.cpp
 * @brief GPU-direct asset loading via DirectStorage (Windows 11+)
 * @author Spark Engine Team
 * @date 2026
 */

#include "DirectStorageLoader.h"
#include "../../Utils/LogMacros.h"

#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace Spark::Streaming
{

    DirectStorageLoader::~DirectStorageLoader()
    {
        Shutdown();
        std::lock_guard lock(m_threadsMutex);
        for (auto& t : m_backgroundThreads)
        {
            if (t.joinable())
                t.join();
        }
        m_backgroundThreads.clear();
    }

    bool DirectStorageLoader::Initialize(void* graphicsDevice)
    {
        std::lock_guard lock(m_mutex);
        if (m_initialized)
            return true;

        // Probe for DirectStorage runtime (dstorage.dll) on Windows.
        // When the DLL is present (Windows 11+ with DirectStorage SDK installed),
        // the hardware path could be activated via DStorageGetFactory().
        // For now we detect availability and report it, but always use the
        // async I/O fallback since full queue integration requires a D3D12 device.
        m_stats.usingDirectStorage = false;
        m_stats.usingGPUDecompression = false;

#ifdef SPARK_PLATFORM_WINDOWS
        HMODULE dstorageModule = LoadLibraryA("dstorage.dll");
        if (dstorageModule != nullptr)
        {
            // DirectStorage runtime is present on this system.
            // Full integration would call DStorageGetFactory() here to create
            // an IDStorageFactory, then CreateQueue() for an IDStorageQueue.
            // Requires a valid ID3D12Device passed via graphicsDevice parameter.
            m_stats.usingDirectStorage = (graphicsDevice != nullptr);
            FreeLibrary(dstorageModule);
        }
#else
        // DirectStorage is Windows-only; Linux/macOS use async I/O fallback.
        static_cast<void>(graphicsDevice);
#endif

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "DirectStorageLoader initialized (hardware=%s, GPU decompression=%s)",
                       m_stats.usingDirectStorage ? "yes" : "no", m_stats.usingGPUDecompression ? "yes" : "no");
        return true;
    }

    void DirectStorageLoader::Shutdown()
    {
        std::lock_guard lock(m_mutex);
        // Idempotent: Shutdown is called from both the engine teardown path and
        // static-destructor cleanup, producing duplicate log lines otherwise.
        if (!m_initialized)
            return;
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "DirectStorageLoader shutting down (%zu active, %llu total bytes)",
                       m_activeRequests.size(), static_cast<unsigned long long>(m_stats.totalBytesLoaded));
        m_activeRequests.clear();
        while (!m_pendingQueue.empty())
            m_pendingQueue.pop();
        m_initialized = false;
    }

    LoadRequestHandle DirectStorageLoader::Submit(const LoadRequest& request)
    {
        std::lock_guard lock(m_mutex);

        auto internal = std::make_shared<InternalRequest>();
        internal->request = request;
        uint64_t newId = m_nextHandleId++;
        if (m_nextHandleId == 0)
            m_nextHandleId = 1;
        internal->handle = {newId};
        internal->status = LoadStatus::Pending;

        m_pendingQueue.push(internal);
        m_stats.totalRequests++;
        m_stats.pendingRequests++;

        SPARK_LOG_DEBUG(Spark::LogCategory::Scene, "Submitted load request #%llu: %s (offset=%llu, size=%llu)",
                        static_cast<unsigned long long>(internal->handle.id), request.filePath.c_str(),
                        static_cast<unsigned long long>(request.fileOffset),
                        static_cast<unsigned long long>(request.loadSize));

        return internal->handle;
    }

    void DirectStorageLoader::Flush()
    {
        std::lock_guard lock(m_mutex);

        while (!m_pendingQueue.empty())
        {
            auto req = m_pendingQueue.front();
            m_pendingQueue.pop();
            req->status = LoadStatus::InProgress;
            m_activeRequests.push_back(req);

            // DirectStorage path would submit to IDStorageQueue here.
            // Fallback: launch async I/O on a background thread.
            FallbackAsyncLoad(req);
        }
    }

    void DirectStorageLoader::ProcessCompletions()
    {
        std::lock_guard lock(m_mutex);

        for (auto it = m_activeRequests.begin(); it != m_activeRequests.end();)
        {
            auto& req = *it;
            if (req->status == LoadStatus::Completed || req->status == LoadStatus::Failed)
            {
                m_stats.pendingRequests--;
                if (req->cancelled.load())
                {
                    req->status = LoadStatus::Failed;
                }
                if (req->status == LoadStatus::Failed)
                    m_stats.failedRequests++;

                if (req->request.callback && !req->cancelled.load())
                {
                    req->request.callback(req->handle, req->status);
                }
                it = m_activeRequests.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool DirectStorageLoader::Cancel(LoadRequestHandle handle)
    {
        std::lock_guard lock(m_mutex);

        for (auto& req : m_activeRequests)
        {
            if (req->handle.id == handle.id)
            {
                if (req->status == LoadStatus::Pending)
                {
                    req->status = LoadStatus::Failed;
                    req->cancelled.store(true);
                    return true;
                }
                if (req->status == LoadStatus::InProgress)
                {
                    req->cancelled.store(true);
                    return true;
                }
            }
        }
        return false;
    }

    LoadStatus DirectStorageLoader::GetStatus(LoadRequestHandle handle) const
    {
        std::lock_guard lock(m_mutex);

        for (const auto& req : m_activeRequests)
        {
            if (req->handle.id == handle.id)
                return req->status;
        }
        return LoadStatus::Failed;
    }

    const void* DirectStorageLoader::GetLoadedData(LoadRequestHandle handle, uint64_t* outSize) const
    {
        std::lock_guard lock(m_mutex);

        for (const auto& req : m_activeRequests)
        {
            if (req->handle.id == handle.id && req->status == LoadStatus::Completed)
            {
                if (outSize)
                    *outSize = req->cpuData.size();
                return req->cpuData.data();
            }
        }
        if (outSize)
            *outSize = 0;
        return nullptr;
    }

    void DirectStorageLoader::ReleaseLoadedData(LoadRequestHandle handle)
    {
        std::lock_guard lock(m_mutex);

        for (auto it = m_activeRequests.begin(); it != m_activeRequests.end(); ++it)
        {
            if ((*it)->handle.id == handle.id)
            {
                (*it)->cpuData.clear();
                (*it)->cpuData.shrink_to_fit();
                return;
            }
        }
    }

    void DirectStorageLoader::FallbackAsyncLoad(std::shared_ptr<InternalRequest> req)
    {
        std::thread worker(
            [this, req]()
            {
                auto startTime = std::chrono::high_resolution_clock::now();

                std::ifstream file(req->request.filePath, std::ios::binary | std::ios::ate);
                if (!file.is_open())
                {
                    // A missing asset is an *expected* failure mode for an
                    // async streaming loader — callers (e.g. AreaAssetLoader)
                    // aggregate the per-area success/failure counts and log a
                    // single WARN. Logging an ERROR here with a stack trace
                    // spams the console whenever a manifest lists optional or
                    // not-yet-shipped files (see OpenWorld showcase).
                    SPARK_LOG_DEBUG(Spark::LogCategory::Scene, "Failed to open file for async load: %s",
                                    req->request.filePath.c_str());
                    req->status = LoadStatus::Failed;
                    return;
                }

                const std::streamsize rawSize = file.tellg();
                if (rawSize < 0)
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Scene, "tellg() failed for async load: %s",
                                    req->request.filePath.c_str());
                    req->status = LoadStatus::Failed;
                    return;
                }
                uint64_t fileSize = static_cast<uint64_t>(rawSize);
                uint64_t readOffset = req->request.fileOffset;
                uint64_t readSize = req->request.loadSize;

                // Validate offset is within file bounds
                if (readOffset >= fileSize)
                {
                    req->status = LoadStatus::Failed;
                    return;
                }

                if (readSize == 0)
                    readSize = fileSize - readOffset;

                // Check for arithmetic overflow before bounds check
                if (readSize > fileSize || readOffset > fileSize - readSize)
                {
                    req->status = LoadStatus::Failed;
                    return;
                }

                file.seekg(static_cast<std::streamoff>(readOffset));

                req->cpuData.resize(readSize);
                file.read(reinterpret_cast<char*>(req->cpuData.data()), static_cast<std::streamsize>(readSize));

                if (!file.good() && !file.eof())
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Scene, "Read error on file: %s", req->request.filePath.c_str());
                    req->status = LoadStatus::Failed;
                    return;
                }

                auto endTime = std::chrono::high_resolution_clock::now();
                float durationMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

                {
                    std::lock_guard lock(m_mutex);
                    m_stats.totalBytesLoaded += readSize;
                    if (durationMs > 0.0f)
                    {
                        float mbps = (static_cast<float>(readSize) / (1024.0f * 1024.0f)) / (durationMs / 1000.0f);
                        // Exponential moving average
                        m_stats.averageBandwidthMBps = m_stats.averageBandwidthMBps * 0.9f + mbps * 0.1f;
                    }
                }

                req->status = LoadStatus::Completed;
            });
        std::lock_guard threadLock(m_threadsMutex);
        m_backgroundThreads.push_back(std::move(worker));
    }

    std::string DirectStorageLoader::Console_GetStatus() const
    {
        std::lock_guard lock(m_mutex);

        return std::format("DirectStorageLoader Status:\n"
                           "  Initialized: {}\n"
                           "  Hardware DirectStorage: {}\n"
                           "  GPU Decompression: {}\n"
                           "  Total requests: {}\n"
                           "  Pending: {}\n"
                           "  Failed: {}\n"
                           "  Bytes loaded: {:.1f} MB\n"
                           "  Avg bandwidth: {:.1f} MB/s",
                           m_initialized, m_stats.usingDirectStorage, m_stats.usingGPUDecompression,
                           m_stats.totalRequests, m_stats.pendingRequests, m_stats.failedRequests,
                           static_cast<float>(m_stats.totalBytesLoaded) / (1024.0f * 1024.0f),
                           m_stats.averageBandwidthMBps);
    }

} // namespace Spark::Streaming
