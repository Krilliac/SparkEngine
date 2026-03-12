/**
 * @file SceneTransitionManager.h
 * @brief Scene streaming, transitions, and additive loading system
 * @author Spark Engine Team
 * @date 2026
 *
 * Extends the base SceneManager with:
 * - Async scene loading with progress callbacks
 * - Scene transitions (fade, loading screen, crossfade)
 * - Additive scene loading (load scenes on top of existing ones)
 * - Scene unloading with reference counting
 * - Loading screen integration
 */

#pragma once

#include "../../Core/Platform.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Spark::Streaming
{

    // ============================================================================
    // Scene Load State
    // ============================================================================

    enum class SceneLoadState : uint8_t
    {
        Unloaded,
        Loading,
        Loaded,
        Unloading,
        Failed
    };

    enum class TransitionType : uint8_t
    {
        None,      ///< Instant swap (no visual transition)
        Fade,      ///< Fade to black, swap, fade in
        Crossfade, ///< Overlap both scenes briefly
        Loading    ///< Show loading screen during swap
    };

    // ============================================================================
    // Scene Handle
    // ============================================================================

    using SceneID = uint32_t;
    constexpr SceneID INVALID_SCENE = 0;

    struct SceneInfo
    {
        SceneID id = INVALID_SCENE;
        std::string name;
        std::string filePath;
        SceneLoadState state = SceneLoadState::Unloaded;
        float loadProgress = 0.0f; ///< 0.0 to 1.0 during loading
        bool isAdditive = false;   ///< Whether loaded additively
        int referenceCount = 0;    ///< For additive scenes
    };

    // ============================================================================
    // Callbacks
    // ============================================================================

    /// @brief Called during loading with progress (0.0 - 1.0)
    using LoadProgressCallback = std::function<void(SceneID id, float progress)>;

    /// @brief Called when loading completes (success or failure)
    using LoadCompleteCallback = std::function<void(SceneID id, bool success)>;

    /// @brief Called during transitions with blend factor (0.0 - 1.0)
    using TransitionCallback = std::function<void(float blend)>;

    // ============================================================================
    // Transition Configuration
    // ============================================================================

    struct TransitionConfig
    {
        TransitionType type = TransitionType::Fade;
        float fadeOutDuration = 0.5f;    ///< Seconds to fade to black
        float fadeInDuration = 0.5f;     ///< Seconds to fade from black
        float minimumLoadTime = 0.0f;    ///< Minimum seconds to show loading screen
        TransitionCallback onTransition; ///< Visual transition callback
    };

    // ============================================================================
    // SceneTransitionManager
    // ============================================================================

    /**
     * @brief Manages scene loading, unloading, and transitions
     *
     * Sits on top of SceneManager and adds async loading with progress,
     * visual transitions, and additive scene support.
     */
    class SceneTransitionManager
    {
      public:
        SceneTransitionManager();
        ~SceneTransitionManager();

        /// @brief Update transitions and loading state (call each frame)
        void Update(float deltaTime);

        // =====================================================================
        // Scene Loading
        // =====================================================================

        /**
         * @brief Load a scene asynchronously, replacing the current scene
         *
         * Unloads the current primary scene, loads the new scene on a
         * background thread, and applies the specified transition.
         *
         * @param filePath  Path to the scene file
         * @param transition Visual transition configuration
         * @param onProgress  Progress callback (optional)
         * @param onComplete  Completion callback (optional)
         * @return SceneID for the scene being loaded, or INVALID_SCENE on error
         */
        SceneID LoadScene(const std::string& filePath, const TransitionConfig& transition = {},
                          LoadProgressCallback onProgress = nullptr, LoadCompleteCallback onComplete = nullptr);

        /**
         * @brief Load a scene additively (on top of existing scenes)
         *
         * Does not unload the current scene. Useful for streaming open worlds,
         * loading UI overlay scenes, or compositing multiple levels.
         *
         * @param filePath  Path to the scene file
         * @param onProgress Progress callback (optional)
         * @param onComplete Completion callback (optional)
         * @return SceneID for the additive scene
         */
        SceneID LoadSceneAdditive(const std::string& filePath, LoadProgressCallback onProgress = nullptr,
                                  LoadCompleteCallback onComplete = nullptr);

        /**
         * @brief Unload an additively-loaded scene
         * @param id SceneID returned from LoadSceneAdditive
         */
        void UnloadScene(SceneID id);

        // =====================================================================
        // Queries
        // =====================================================================

        /// @brief Get the currently active primary scene
        SceneID GetActiveScene() const { return m_activeScene; }

        /// @brief Get info about a loaded/loading scene
        const SceneInfo* GetSceneInfo(SceneID id) const;

        /// @brief Check if any scene is currently loading
        bool IsLoading() const;

        /// @brief Check if a transition is in progress
        bool IsTransitioning() const { return m_transitioning; }

        /// @brief Get overall loading progress (0.0 - 1.0)
        float GetLoadProgress() const;

        /// @brief Get list of all loaded scenes (primary + additive)
        std::vector<SceneID> GetLoadedScenes() const;

        /// @brief Console status string
        std::string Console_GetStatus() const;

      private:
        struct LoadRequest
        {
            SceneID id = INVALID_SCENE;
            std::string filePath;
            bool isAdditive = false;
            TransitionConfig transition;
            LoadProgressCallback onProgress;
            LoadCompleteCallback onComplete;
        };

        void ProcessLoadQueue();
        void ExecuteLoad(const LoadRequest& request);
        void BeginTransition(const TransitionConfig& config);
        void UpdateTransition(float deltaTime);
        void FinishTransition();

        SceneID AllocateSceneID();

        // Scene registry
        std::unordered_map<SceneID, SceneInfo> m_scenes;
        SceneID m_activeScene = INVALID_SCENE;
        SceneID m_pendingScene = INVALID_SCENE;
        SceneID m_nextSceneID = 1;

        // Loading
        std::vector<LoadRequest> m_loadQueue;
        std::thread m_loadThread;
        std::atomic<bool> m_loadingActive{false};
        std::atomic<float> m_loadProgress{0.0f};
        mutable std::mutex m_mutex;

        // Transition state
        bool m_transitioning = false;
        TransitionConfig m_currentTransition;
        float m_transitionTimer = 0.0f;
        enum class TransitionPhase
        {
            FadeOut,
            Loading,
            FadeIn,
            Done
        } m_transitionPhase = TransitionPhase::Done;
    };

} // namespace Spark::Streaming
