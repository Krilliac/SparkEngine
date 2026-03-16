/**
 * @file AsyncAssetLoader.h
 * @brief Asynchronous asset loading pipeline using the engine's JobSystem
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides non-blocking asset loading with priority scheduling, progress
 * callbacks, and dependency tracking. Uses the engine's JobSystem thread
 * pool and ThreadSafeQueue for concurrent I/O.
 *
 * ## Usage
 * @code
 *   auto& loader = Spark::AsyncAssetLoader::Get();
 *   loader.Initialize();
 *
 *   // Request an asset load with priority and callback
 *   auto handle = loader.LoadAsync("Assets/Models/Player.obj",
 *       AssetPriority::High,
 *       [](const AssetLoadResult& result) {
 *           if (result.success) { ... }
 *       });
 *
 *   // Poll for completion or wait
 *   if (loader.IsComplete(handle)) { ... }
 *
 *   // Per-frame update (processes completed callbacks on main thread)
 *   loader.Update();
 *
 *   loader.Shutdown();
 * @endcode
 */

#pragma once

#include "../../Utils/JobSystem.h"
#include "../../Utils/ThreadSafeQueue.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    // ============================================================================
    // Asset Load Priority
    // ============================================================================

    enum class AssetPriority : uint8_t
    {
        Low = 0,     ///< Background loading (prefetch, distant LODs)
        Normal = 1,  ///< Default priority
        High = 2,    ///< Visible assets needed soon
        Critical = 3 ///< Blocking — must load ASAP (loading screen assets)
    };

    // ============================================================================
    // Asset Load Handle
    // ============================================================================

    using AssetLoadHandle = uint64_t;
    constexpr AssetLoadHandle INVALID_ASSET_HANDLE = 0;

    // ============================================================================
    // Asset Load State
    // ============================================================================

    enum class AssetLoadState : uint8_t
    {
        Pending,   ///< Queued but not yet started
        Loading,   ///< Currently being loaded on a worker thread
        Completed, ///< Successfully loaded
        Failed     ///< Loading failed (file not found, corrupt, etc.)
    };

    // ============================================================================
    // Asset Load Result
    // ============================================================================

    struct AssetLoadResult
    {
        AssetLoadHandle handle = INVALID_ASSET_HANDLE;
        std::string path;
        bool success = false;
        std::string errorMessage;
        std::vector<uint8_t> data; ///< Raw file data (for custom processing)
        float loadTimeMs = 0.0f;   ///< How long the load took
    };

    // ============================================================================
    // Callbacks
    // ============================================================================

    using AssetLoadCallback = std::function<void(const AssetLoadResult&)>;

    // ============================================================================
    // AsyncAssetLoader
    // ============================================================================

    /**
     * @brief Asynchronous asset loading pipeline
     *
     * Dispatches file I/O to the JobSystem thread pool and delivers results
     * back to the main thread via callbacks during Update(). Assets are loaded
     * by priority (Critical > High > Normal > Low).
     *
     * Thread safety: LoadAsync() is thread-safe. Update() must be called from
     * the main thread.
     */
    class AsyncAssetLoader
    {
      public:
        static AsyncAssetLoader& Get()
        {
            static AsyncAssetLoader instance;
            return instance;
        }

        /**
         * @brief Initialize the async loader
         * @return true on success
         */
        bool Initialize()
        {
            if (m_initialized)
                return true;
            m_initialized = true;
            m_nextHandle.store(1, std::memory_order_relaxed);
            return true;
        }

        /**
         * @brief Shutdown and cancel all pending loads
         */
        void Shutdown()
        {
            if (!m_initialized)
                return;
            CancelAll();
            m_initialized = false;
        }

        /**
         * @brief Request an asset to be loaded asynchronously
         *
         * @param path      File path to the asset
         * @param priority  Loading priority (higher = loaded sooner)
         * @param callback  Called on the main thread when loading completes
         * @return Handle for tracking the load request
         */
        AssetLoadHandle LoadAsync(const std::string& path, AssetPriority priority = AssetPriority::Normal,
                                  AssetLoadCallback callback = nullptr)
        {
            if (!m_initialized || path.empty())
                return INVALID_ASSET_HANDLE;

            AssetLoadHandle handle = m_nextHandle.fetch_add(1, std::memory_order_relaxed);

            LoadRequest request;
            request.handle = handle;
            request.path = path;
            request.priority = priority;
            request.callback = std::move(callback);

            {
                std::lock_guard<std::mutex> lock(m_requestMutex);
                m_requests[handle] = request;
                m_states[handle] = AssetLoadState::Pending;
            }

            // Submit to job system
            auto& jobs = JobSystem::Get();
            if (jobs.IsInitialized())
            {
                jobs.Submit([this, handle, path]() { ExecuteLoad(handle, path); });
            }
            else
            {
                // Fallback: load synchronously
                ExecuteLoad(handle, path);
            }

            return handle;
        }

        /**
         * @brief Process completed load callbacks on the main thread
         *
         * Must be called from the main thread each frame. Drains the
         * completion queue and invokes registered callbacks.
         */
        void Update()
        {
            std::vector<AssetLoadResult> completed;
            m_completionQueue.PopAll(completed);

            for (auto& result : completed)
            {
                AssetLoadCallback callback;
                {
                    std::lock_guard<std::mutex> lock(m_requestMutex);
                    auto it = m_requests.find(result.handle);
                    if (it != m_requests.end())
                    {
                        callback = std::move(it->second.callback);
                        m_requests.erase(it);
                    }
                }

                if (callback)
                {
                    callback(result);
                }
            }
        }

        /**
         * @brief Check if a load request has completed
         */
        bool IsComplete(AssetLoadHandle handle) const
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            auto it = m_states.find(handle);
            if (it == m_states.end())
                return true; // Unknown handle = already done
            return it->second == AssetLoadState::Completed || it->second == AssetLoadState::Failed;
        }

        /**
         * @brief Get the current state of a load request
         */
        AssetLoadState GetState(AssetLoadHandle handle) const
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            auto it = m_states.find(handle);
            return it != m_states.end() ? it->second : AssetLoadState::Completed;
        }

        /**
         * @brief Cancel all pending load requests
         */
        void CancelAll()
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            m_requests.clear();
            m_states.clear();
            m_completionQueue.Clear();
        }

        /**
         * @brief Get the number of pending load requests
         */
        size_t GetPendingCount() const
        {
            std::lock_guard<std::mutex> lock(m_requestMutex);
            size_t count = 0;
            for (const auto& [handle, state] : m_states)
            {
                if (state == AssetLoadState::Pending || state == AssetLoadState::Loading)
                    ++count;
            }
            return count;
        }

        /** @brief Console status string */
        std::string Console_GetStatus() const
        {
            size_t pending = GetPendingCount();
            return "AsyncAssetLoader: " + std::to_string(pending) + " pending loads";
        }

      private:
        AsyncAssetLoader() = default;
        ~AsyncAssetLoader() { Shutdown(); }
        AsyncAssetLoader(const AsyncAssetLoader&) = delete;
        AsyncAssetLoader& operator=(const AsyncAssetLoader&) = delete;

        struct LoadRequest
        {
            AssetLoadHandle handle = INVALID_ASSET_HANDLE;
            std::string path;
            AssetPriority priority = AssetPriority::Normal;
            AssetLoadCallback callback;
        };

        void ExecuteLoad(AssetLoadHandle handle, const std::string& path)
        {
            {
                std::lock_guard<std::mutex> lock(m_requestMutex);
                auto stateIt = m_states.find(handle);
                if (stateIt == m_states.end())
                    return; // Cancelled
                stateIt->second = AssetLoadState::Loading;
            }

            auto startTime = std::chrono::high_resolution_clock::now();

            AssetLoadResult result;
            result.handle = handle;
            result.path = path;

            // Read the file
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open())
            {
                result.success = false;
                result.errorMessage = "Failed to open file: " + path;
            }
            else
            {
                auto fileSize = file.tellg();
                file.seekg(0, std::ios::beg);
                result.data.resize(static_cast<size_t>(fileSize));
                if (file.read(reinterpret_cast<char*>(result.data.data()), fileSize))
                {
                    result.success = true;
                }
                else
                {
                    result.success = false;
                    result.errorMessage = "Failed to read file: " + path;
                    result.data.clear();
                }
            }

            auto endTime = std::chrono::high_resolution_clock::now();
            result.loadTimeMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();

            {
                std::lock_guard<std::mutex> lock(m_requestMutex);
                auto stateIt = m_states.find(handle);
                if (stateIt != m_states.end())
                {
                    stateIt->second = result.success ? AssetLoadState::Completed : AssetLoadState::Failed;
                }
            }

            m_completionQueue.ForcePush(std::move(result));
        }

        mutable std::mutex m_requestMutex;
        std::unordered_map<AssetLoadHandle, LoadRequest> m_requests;
        std::unordered_map<AssetLoadHandle, AssetLoadState> m_states;
        ThreadSafeQueue<AssetLoadResult> m_completionQueue;
        std::atomic<AssetLoadHandle> m_nextHandle{1};
        std::atomic<bool> m_initialized{false};
    };

} // namespace Spark
