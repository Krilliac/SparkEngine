/**
 * @file SceneManager.cpp
 * @brief Implementation of the scene management system
 *
 * Owns the current SceneFile and delegates load/save to SceneSerializer.
 * CreateNewScene populates the scene with a default camera and directional light.
 */

#include "SceneManager.h"
#include "Utils/Validate.h"

#include <cstring>
#include <ctime>

namespace SparkEditor
{

    SceneManager::SceneManager() = default;

    SceneManager::~SceneManager() = default;

    bool SceneManager::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "SceneManager initialized");
        m_serializer.SetPrettyPrintJSON(true);
        m_isInitialized = true;
        return true;
    }

    void SceneManager::Update(float /*deltaTime*/) {}

    void SceneManager::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "SceneManager shutdown");
        m_currentScene.reset();
        m_isInitialized = false;
    }

    bool SceneManager::CreateNewScene(const std::string& sceneName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !sceneName.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "Creating new scene '%s'", sceneName.c_str());

        m_currentScene = std::make_unique<SceneFile>();
        auto& header = m_currentScene->header;
        header.magic = SCENE_FILE_MAGIC;
        header.version = SCENE_FILE_VERSION;
        header.timestamp = static_cast<uint64_t>(std::time(nullptr));
        std::strncpy(header.sceneName, sceneName.c_str(), sizeof(header.sceneName) - 1);
        header.sceneName[sizeof(header.sceneName) - 1] = '\0';

        // Default camera
        {
            SceneObject cam;
            cam.id = m_currentScene->GetNextObjectID();
            cam.name = "Main Camera";
            cam.active = true;
            cam.transform.position = {0.0f, 2.0f, -5.0f};
            cam.componentTypes.push_back(ComponentType::CAMERA);
            m_currentScene->objects.push_back(std::move(cam));

            Component camComp;
            camComp.type = ComponentType::CAMERA;
            camComp.objectID = m_currentScene->objects.back().id;
            Camera camData;
            camData.fieldOfView = 60.0f;
            camData.nearPlane = 0.1f;
            camData.farPlane = 1000.0f;
            camData.isMainCamera = true;
            camComp.SetData(camData);
            m_currentScene->components.push_back(std::move(camComp));
        }

        // Default directional light
        {
            SceneObject light;
            light.id = m_currentScene->GetNextObjectID();
            light.name = "Directional Light";
            light.active = true;
            light.transform.position = {0.0f, 10.0f, 0.0f};
            light.componentTypes.push_back(ComponentType::LIGHT);
            m_currentScene->objects.push_back(std::move(light));

            Component lightComp;
            lightComp.type = ComponentType::LIGHT;
            lightComp.objectID = m_currentScene->objects.back().id;
            Light lightData;
            lightData.type = Light::DIRECTIONAL;
            lightData.color = {1.0f, 1.0f, 1.0f};
            lightData.intensity = 1.0f;
            lightComp.SetData(lightData);
            m_currentScene->components.push_back(std::move(lightComp));
        }

        // Default ground plane
        {
            SceneObject ground;
            ground.id = m_currentScene->GetNextObjectID();
            ground.name = "Ground";
            ground.active = true;
            ground.transform.scale = {10.0f, 0.1f, 10.0f};
            ground.componentTypes.push_back(ComponentType::MESH_RENDERER);
            m_currentScene->objects.push_back(std::move(ground));

            Component meshComp;
            meshComp.type = ComponentType::MESH_RENDERER;
            meshComp.objectID = m_currentScene->objects.back().id;
            MeshRenderer meshData;
            meshData.meshAssetPath = "Primitives/Cube";
            meshComp.SetData(meshData);
            m_currentScene->components.push_back(std::move(meshComp));
        }

        m_currentScene->UpdateHeader();
        m_currentScenePath = sceneName + ".sparkscene";
        m_hasUnsavedChanges = true;

        if (m_onSceneChanged)
            m_onSceneChanged();

        return true;
    }

    bool SceneManager::LoadScene(const std::string& scenePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !scenePath.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "Loading scene from '%s'", scenePath.c_str());

        auto loadedScene = std::make_unique<SceneFile>();
        auto result = m_serializer.LoadScene(scenePath, *loadedScene);
        if (!result.success)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Scene, "Failed to load scene: %s", result.errorMessage.c_str());
            return false;
        }

        m_currentScene = std::move(loadedScene);
        m_currentScenePath = scenePath;
        m_hasUnsavedChanges = false;

        if (m_onSceneChanged)
            m_onSceneChanged();

        return true;
    }

    bool SceneManager::SaveScene(const std::string& scenePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, !scenePath.empty(), false);
        SPARK_VALIDATE_RET(Spark::LogCategory::Scene, m_currentScene != nullptr, false);
        SPARK_LOG_INFO(Spark::LogCategory::Scene, "Saving scene to '%s'", scenePath.c_str());

        m_currentScene->header.timestamp = static_cast<uint64_t>(std::time(nullptr));
        m_currentScene->UpdateHeader();

        auto result = m_serializer.SaveScene(*m_currentScene, scenePath);
        if (!result.success)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Scene, "Failed to save scene: %s", result.errorMessage.c_str());
            return false;
        }

        m_currentScenePath = scenePath;
        m_hasUnsavedChanges = false;
        return true;
    }

} // namespace SparkEditor
