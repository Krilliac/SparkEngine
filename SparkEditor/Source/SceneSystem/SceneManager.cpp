/**
 * @file SceneManager.cpp
 * @brief Implementation of the scene management system
 * @author Spark Engine Team
 * @date 2025
 */

#include "SceneManager.h"
#include "Utils/Validate.h"
#include <iostream>

namespace SparkEditor
{

    SceneManager::SceneManager() = default;

    SceneManager::~SceneManager() = default;

    bool SceneManager::Initialize()
    {
        std::cout << "SceneManager::Initialize()\n";
        m_isInitialized = true;
        return true;
    }

    void SceneManager::Update(float deltaTime)
    {
        // Scene manager update logic will go here
        (void)deltaTime; // Suppress unused parameter warning
    }

    void SceneManager::Shutdown()
    {
        std::cout << "SceneManager::Shutdown()\n";
        m_isInitialized = false;
    }

    bool SceneManager::CreateNewScene(const std::string& sceneName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !sceneName.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "Creating new scene '%s'", sceneName.c_str());
        std::cout << "Creating new scene: " << sceneName << "\n";
        m_currentScenePath = sceneName + ".scene";
        m_hasUnsavedChanges = true;
        return true;
    }

    bool SceneManager::LoadScene(const std::string& scenePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !scenePath.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "Loading scene from '%s'", scenePath.c_str());
        std::cout << "Loading scene: " << scenePath << "\n";
        m_currentScenePath = scenePath;
        m_hasUnsavedChanges = false;
        return true;
    }

    bool SceneManager::SaveScene(const std::string& scenePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !scenePath.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "Saving scene to '%s'", scenePath.c_str());
        std::cout << "Saving scene: " << scenePath << "\n";
        m_currentScenePath = scenePath;
        m_hasUnsavedChanges = false;
        return true;
    }

} // namespace SparkEditor