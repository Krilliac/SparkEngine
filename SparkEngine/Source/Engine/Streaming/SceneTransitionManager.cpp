/**
 * @file SceneTransitionManager.cpp
 * @brief Scene streaming and transition implementation
 */

#include "SceneTransitionManager.h"

#include <algorithm>
#include <sstream>

namespace Spark::Streaming
{

    SceneTransitionManager::SceneTransitionManager() = default;

    SceneTransitionManager::~SceneTransitionManager()
    {
        if (m_loadThread.joinable())
            m_loadThread.join();
    }

    void SceneTransitionManager::Update(float deltaTime)
    {
        // Process transition animation
        if (m_transitioning)
        {
            UpdateTransition(deltaTime);
        }

        // Check for completed loads
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& [id, info] : m_scenes)
        {
            if (info.state == SceneLoadState::Loading && m_loadProgress >= 1.0f && !m_loadingActive)
            {
                info.state = SceneLoadState::Loaded;
                info.loadProgress = 1.0f;
            }
        }
    }

    SceneID SceneTransitionManager::LoadScene(const std::string& filePath, const TransitionConfig& transition,
                                              LoadProgressCallback onProgress, LoadCompleteCallback onComplete)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        SceneID id = AllocateSceneID();

        SceneInfo info;
        info.id = id;
        info.filePath = filePath;
        info.state = SceneLoadState::Loading;
        info.isAdditive = false;
        // Extract name from file path
        auto lastSlash = filePath.find_last_of("/\\");
        auto lastDot = filePath.find_last_of('.');
        if (lastSlash != std::string::npos)
            info.name = filePath.substr(lastSlash + 1, lastDot - lastSlash - 1);
        else
            info.name = filePath.substr(0, lastDot);

        m_scenes[id] = info;
        m_pendingScene = id;

        LoadRequest request;
        request.id = id;
        request.filePath = filePath;
        request.isAdditive = false;
        request.transition = transition;
        request.onProgress = std::move(onProgress);
        request.onComplete = std::move(onComplete);
        m_loadQueue.push_back(std::move(request));

        // Begin transition if configured
        if (transition.type != TransitionType::None)
        {
            BeginTransition(transition);
        }

        ProcessLoadQueue();
        return id;
    }

    SceneID SceneTransitionManager::LoadSceneAdditive(const std::string& filePath, LoadProgressCallback onProgress,
                                                      LoadCompleteCallback onComplete)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        SceneID id = AllocateSceneID();

        SceneInfo info;
        info.id = id;
        info.filePath = filePath;
        info.state = SceneLoadState::Loading;
        info.isAdditive = true;
        info.referenceCount = 1;
        auto lastSlash = filePath.find_last_of("/\\");
        auto lastDot = filePath.find_last_of('.');
        if (lastSlash != std::string::npos)
            info.name = filePath.substr(lastSlash + 1, lastDot - lastSlash - 1);
        else
            info.name = filePath.substr(0, lastDot);

        m_scenes[id] = info;

        LoadRequest request;
        request.id = id;
        request.filePath = filePath;
        request.isAdditive = true;
        request.onProgress = std::move(onProgress);
        request.onComplete = std::move(onComplete);
        m_loadQueue.push_back(std::move(request));

        ProcessLoadQueue();
        return id;
    }

    void SceneTransitionManager::UnloadScene(SceneID id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_scenes.find(id);
        if (it == m_scenes.end())
            return;

        if (it->second.isAdditive)
        {
            it->second.referenceCount--;
            if (it->second.referenceCount <= 0)
            {
                it->second.state = SceneLoadState::Unloaded;
                m_scenes.erase(it);
            }
        }
    }

    const SceneInfo* SceneTransitionManager::GetSceneInfo(SceneID id) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_scenes.find(id);
        return it != m_scenes.end() ? &it->second : nullptr;
    }

    bool SceneTransitionManager::IsLoading() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, info] : m_scenes)
        {
            if (info.state == SceneLoadState::Loading)
                return true;
        }
        return false;
    }

    float SceneTransitionManager::GetLoadProgress() const
    {
        return m_loadProgress.load();
    }

    std::vector<SceneID> SceneTransitionManager::GetLoadedScenes() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<SceneID> result;
        for (const auto& [id, info] : m_scenes)
        {
            if (info.state == SceneLoadState::Loaded)
                result.push_back(id);
        }
        return result;
    }

    std::string SceneTransitionManager::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream ss;
        ss << "Scene Transition Manager:\n";
        ss << "  Active Scene: " << m_activeScene << "\n";
        ss << "  Transitioning: " << (m_transitioning ? "YES" : "NO") << "\n";
        ss << "  Loading: " << (m_loadingActive ? "YES" : "NO") << "\n";
        ss << "  Scenes (" << m_scenes.size() << "):\n";

        for (const auto& [id, info] : m_scenes)
        {
            ss << "    [" << id << "] " << info.name << " - ";
            switch (info.state)
            {
            case SceneLoadState::Unloaded:
                ss << "UNLOADED";
                break;
            case SceneLoadState::Loading:
                ss << "LOADING (" << static_cast<int>(info.loadProgress * 100) << "%)";
                break;
            case SceneLoadState::Loaded:
                ss << "LOADED";
                break;
            case SceneLoadState::Unloading:
                ss << "UNLOADING";
                break;
            case SceneLoadState::Failed:
                ss << "FAILED";
                break;
            }
            if (info.isAdditive)
                ss << " [additive, refs=" << info.referenceCount << "]";
            ss << "\n";
        }

        return ss.str();
    }

    // ============================================================================
    // Private
    // ============================================================================

    void SceneTransitionManager::ProcessLoadQueue()
    {
        if (m_loadingActive || m_loadQueue.empty())
            return;

        auto request = std::move(m_loadQueue.front());
        m_loadQueue.erase(m_loadQueue.begin());

        m_loadingActive = true;
        m_loadProgress = 0.0f;

        // Launch background load thread
        if (m_loadThread.joinable())
            m_loadThread.join();

        m_loadThread = std::thread([this, req = std::move(request)]() { ExecuteLoad(req); });
    }

    void SceneTransitionManager::ExecuteLoad(const LoadRequest& request)
    {
        // Simulate progressive loading phases
        // In a real implementation, this would call SceneManager::LoadSceneAsync
        // and report actual progress from the JSON parser / asset loader.

        auto reportProgress = [&](float progress)
        {
            m_loadProgress = progress;
            if (request.onProgress)
                request.onProgress(request.id, progress);

            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_scenes.find(request.id);
            if (it != m_scenes.end())
                it->second.loadProgress = progress;
        };

        // Phase 1: Parse scene file (0% - 30%)
        reportProgress(0.1f);
        // Actual file I/O would happen here via SceneManager
        reportProgress(0.3f);

        // Phase 2: Load assets (30% - 80%)
        reportProgress(0.5f);
        reportProgress(0.8f);

        // Phase 3: Initialize scene objects (80% - 100%)
        reportProgress(0.9f);
        reportProgress(1.0f);

        // Mark as loaded
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_scenes.find(request.id);
            if (it != m_scenes.end())
            {
                it->second.state = SceneLoadState::Loaded;
                it->second.loadProgress = 1.0f;
            }

            // If this is a primary scene load, update active scene
            if (!request.isAdditive)
            {
                // Unload previous active scene
                if (m_activeScene != INVALID_SCENE)
                {
                    auto prevIt = m_scenes.find(m_activeScene);
                    if (prevIt != m_scenes.end() && !prevIt->second.isAdditive)
                    {
                        prevIt->second.state = SceneLoadState::Unloaded;
                        m_scenes.erase(prevIt);
                    }
                }
                m_activeScene = request.id;
            }
        }

        m_loadingActive = false;

        if (request.onComplete)
            request.onComplete(request.id, true);
    }

    void SceneTransitionManager::BeginTransition(const TransitionConfig& config)
    {
        m_transitioning = true;
        m_currentTransition = config;
        m_transitionPhase = TransitionPhase::FadeOut;
        m_transitionTimer = config.fadeOutDuration;
    }

    void SceneTransitionManager::UpdateTransition(float deltaTime)
    {
        m_transitionTimer -= deltaTime;

        float blend = 0.0f;
        switch (m_transitionPhase)
        {
        case TransitionPhase::FadeOut:
            blend = 1.0f - (m_transitionTimer / std::max(m_currentTransition.fadeOutDuration, 0.001f));
            blend = std::clamp(blend, 0.0f, 1.0f);
            if (m_transitionTimer <= 0.0f)
            {
                m_transitionPhase = TransitionPhase::Loading;
                m_transitionTimer = m_currentTransition.minimumLoadTime;
            }
            break;

        case TransitionPhase::Loading:
            blend = 1.0f; // Full black/loading screen
            if (m_transitionTimer <= 0.0f && !m_loadingActive)
            {
                m_transitionPhase = TransitionPhase::FadeIn;
                m_transitionTimer = m_currentTransition.fadeInDuration;
            }
            break;

        case TransitionPhase::FadeIn:
            blend = m_transitionTimer / std::max(m_currentTransition.fadeInDuration, 0.001f);
            blend = std::clamp(blend, 0.0f, 1.0f);
            if (m_transitionTimer <= 0.0f)
            {
                FinishTransition();
                return;
            }
            break;

        case TransitionPhase::Done:
            return;
        }

        if (m_currentTransition.onTransition)
            m_currentTransition.onTransition(blend);
    }

    void SceneTransitionManager::FinishTransition()
    {
        m_transitioning = false;
        m_transitionPhase = TransitionPhase::Done;
        m_transitionTimer = 0.0f;
    }

    SceneID SceneTransitionManager::AllocateSceneID()
    {
        return m_nextSceneID++;
    }

} // namespace Spark::Streaming
