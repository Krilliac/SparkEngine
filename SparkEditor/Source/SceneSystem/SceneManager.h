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

#include <string>
#include <memory>

namespace SparkEditor
{

    /**
 * @brief Scene management system
 * 
 * Handles scene file operations, scene loading/saving, and scene state management.
 */
    class SceneManager
    {
      public:
        /**
     * @brief Constructor
     */
        SceneManager();

        /**
     * @brief Destructor
     */
        ~SceneManager();

        /**
     * @brief Initialize the scene manager
     * @return true if initialization succeeded, false otherwise
     */
        bool Initialize();

        /**
     * @brief Update scene manager
     * @param deltaTime Time elapsed since last frame in seconds
     */
        void Update(float deltaTime);

        /**
     * @brief Shutdown the scene manager
     */
        void Shutdown();

        /**
     * @brief Create a new scene
     * @param sceneName Name of the new scene
     * @return true if scene was created successfully
     */
        bool CreateNewScene(const std::string& sceneName);

        /**
     * @brief Load scene from file
     * @param scenePath Path to the scene file
     * @return true if scene was loaded successfully
     */
        bool LoadScene(const std::string& scenePath);

        /**
     * @brief Save current scene
     * @param scenePath Path where to save the scene
     * @return true if scene was saved successfully
     */
        bool SaveScene(const std::string& scenePath);

        /**
     * @brief Get current scene path
     * @return Path to the currently loaded scene, or empty string if none
     */
        const std::string& GetCurrentScenePath() const { return m_currentScenePath; }

        /**
     * @brief Check if scene has unsaved changes
     * @return true if scene has unsaved changes
     */
        bool HasUnsavedChanges() const { return m_hasUnsavedChanges; }

      private:
        std::string m_currentScenePath;   ///< Path to current scene
        bool m_hasUnsavedChanges = false; ///< Whether scene has unsaved changes
        bool m_isInitialized = false;     ///< Initialization state
    };

} // namespace SparkEditor