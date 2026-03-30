/**
 * @file SceneManager.h
 * @brief Scene management system for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file defines the scene management system that handles loading,
 * saving, and managing scene files and their contents.
 */

#pragma once

#include "SceneFile.h"
#include "SceneSerializer.h"

#include <functional>
#include <memory>
#include <string>

namespace SparkEditor
{

    /**
     * @brief Scene management system
     *
     * Owns the current SceneFile and delegates serialization to SceneSerializer.
     * Panels read/write through GetCurrentScene().
     */
    class SceneManager
    {
      public:
        SceneManager();
        ~SceneManager();

        bool Initialize();
        void Update(float deltaTime);
        void Shutdown();

        /// @brief Create a blank scene with default camera and light.
        bool CreateNewScene(const std::string& sceneName);

        /// @brief Load a scene from disk (auto-detects format).
        bool LoadScene(const std::string& scenePath);

        /// @brief Save the current scene to disk.
        bool SaveScene(const std::string& scenePath);

        const std::string& GetCurrentScenePath() const { return m_currentScenePath; }
        bool HasUnsavedChanges() const { return m_hasUnsavedChanges; }

        /// @brief Get the active scene data (non-owning). Returns nullptr if no scene loaded.
        SceneFile* GetCurrentScene() { return m_currentScene.get(); }
        const SceneFile* GetCurrentScene() const { return m_currentScene.get(); }

        /// @brief Mark the scene as dirty (has unsaved changes).
        void MarkDirty() { m_hasUnsavedChanges = true; }

        /// @brief Register callback for scene change events.
        void SetOnSceneChanged(std::function<void()> callback) { m_onSceneChanged = std::move(callback); }

      private:
        std::unique_ptr<SceneFile> m_currentScene;
        SceneSerializer m_serializer;
        std::string m_currentScenePath;
        bool m_hasUnsavedChanges = false;
        bool m_isInitialized = false;
        std::function<void()> m_onSceneChanged;
    };

} // namespace SparkEditor