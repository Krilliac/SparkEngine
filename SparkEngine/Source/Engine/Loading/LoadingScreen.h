/**
 * @file LoadingScreen.h
 * @brief Async asset loading with progress tracking and loading screen support
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides a loading screen framework that works with the AssetPipeline to
 * display progress during async asset loading. Supports progress callbacks,
 * loading tips, background images, and cancellation.
 *
 * ## Usage
 * @code
 *   LoadingScreen loader;
 *   loader.SetBackgroundImage("Data/Textures/loading_bg.png");
 *   loader.AddLoadingTip("Press SPACE to jump");
 *
 *   loader.BeginLoading("Level 1");
 *   loader.AddTask("meshes", 0.4f, [](){ LoadAllMeshes(); });
 *   loader.AddTask("textures", 0.3f, [](){ LoadAllTextures(); });
 *   loader.AddTask("audio", 0.2f, [](){ LoadAllAudio(); });
 *   loader.AddTask("scripts", 0.1f, [](){ CompileScripts(); });
 *
 *   loader.OnProgress([](float p, const std::string& msg){
 *       UpdateProgressBar(p); UpdateStatusText(msg);
 *   });
 *
 *   loader.Execute(); // Runs tasks, fires callbacks
 * @endcode
 */

#pragma once

#include "../../Utils/Delegate.h"

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

namespace Spark
{

    // =============================================================================
    // LoadingTask
    // =============================================================================

    /**
 * @brief A single loading task with weight and callback.
 */
    struct LoadingTask
    {
        std::string name;              ///< Task name (for display)
        float weight = 1.0f;           ///< Relative weight (for progress calculation)
        std::function<bool()> execute; ///< Task function (returns true on success)
        bool completed = false;        ///< Whether the task has finished
        bool success = false;          ///< Whether the task succeeded
    };

    // =============================================================================
    // LoadingState
    // =============================================================================

    /**
 * @brief Current state of the loading process.
 */
    enum class LoadingState
    {
        Idle,      ///< No loading in progress
        Loading,   ///< Currently loading
        Completed, ///< All tasks finished successfully
        Failed,    ///< One or more tasks failed
        Cancelled  ///< Loading was cancelled
    };

    // =============================================================================
    // LoadingScreen
    // =============================================================================

    /**
 * @class LoadingScreen
 * @brief Manages async loading with progress tracking and display.
 */
    class LoadingScreen
    {
      public:
        LoadingScreen();
        ~LoadingScreen() = default;

        // --- Configuration ---

        /**
     * @brief Set the background image for the loading screen.
     * @param imagePath Path to background texture.
     */
        void SetBackgroundImage(const std::string& imagePath) { m_backgroundImage = imagePath; }

        /**
     * @brief Add a loading tip to rotate through during loading.
     * @param tip Tip text.
     */
        void AddLoadingTip(const std::string& tip);

        /**
     * @brief Set minimum display time (prevents flash for fast loads).
     * @param seconds Minimum time to display loading screen.
     */
        void SetMinimumDisplayTime(float seconds) { m_minDisplayTime = seconds; }

        // --- Task management ---

        /**
     * @brief Begin a new loading session.
     * @param name Session name (e.g. level name).
     */
        void BeginLoading(const std::string& name);

        /**
     * @brief Add a loading task.
     * @param name    Task display name.
     * @param weight  Relative weight for progress (sum of all weights = 100%).
     * @param execute Task function.
     */
        void AddTask(const std::string& name, float weight, std::function<bool()> execute);

        /**
     * @brief Execute all queued tasks synchronously.
     *
     * Fires progress callbacks after each task completes.
     */
        void Execute();

        /**
     * @brief Cancel the current loading process.
     */
        void Cancel();

        // --- Progress ---

        /** @brief Get overall progress (0.0 - 1.0). */
        float GetProgress() const { return m_progress.load(); }

        /** @brief Get the current task name. */
        std::string GetCurrentTaskName() const;

        /** @brief Get the loading state. */
        LoadingState GetState() const { return m_state; }

        /** @brief Get a random loading tip. */
        std::string GetCurrentTip() const;

        // --- Callbacks ---

        /**
     * @brief Register a progress callback.
     * @param callback Called with (progress 0-1, current task name).
     */
        Spark::Delegate<float, const std::string&>::HandlerID OnProgress(
            std::function<void(float, const std::string&)> callback);

        /**
     * @brief Register a completion callback.
     * @param callback Called when all tasks finish.
     */
        Spark::Delegate<bool>::HandlerID OnComplete(std::function<void(bool)> callback);

        // --- Console integration ---

        /** @brief Get loading status (console integration). */
        std::string Console_GetStatus() const;

      private:
        std::string m_sessionName;
        std::string m_backgroundImage;
        std::vector<std::string> m_loadingTips;
        std::vector<LoadingTask> m_tasks;

        std::atomic<float> m_progress{0.0f};
        LoadingState m_state = LoadingState::Idle;
        size_t m_currentTaskIndex = 0;
        float m_minDisplayTime = 1.0f;
        float m_elapsedTime = 0.0f;
        bool m_cancelled = false;

        Spark::Delegate<float, const std::string&> m_progressCallbacks;
        Spark::Delegate<bool> m_completeCallbacks;
        mutable std::mutex m_mutex;
    };

} // namespace Spark
