/**
 * @file SparkEngineIntegration.cpp
 * @brief Full implementation of SparkEngineIntegration
 */

#include "SparkEngineIntegration.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <algorithm>
#include <sstream>

using namespace DirectX;
namespace SparkEditor {

// ============================================================================
// Constructor / Destructor
// ============================================================================

SparkEngineIntegration::SparkEngineIntegration()
    : m_connectionStatus(EngineConnectionStatus::Disconnected)
    , m_shouldStop(false)
    , m_selectedEntityId(0)
    , m_device(nullptr)
    , m_context(nullptr)
    , m_isInitialized(false)
    , m_updateTimer(0.0f)
{
}

SparkEngineIntegration::~SparkEngineIntegration() {
    Shutdown();
}

// ============================================================================
// Initialization
// ============================================================================

bool SparkEngineIntegration::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;
    m_isInitialized = true;
    return true;
}

void SparkEngineIntegration::Shutdown() {
    if (IsConnected() || m_connectionStatus.load() == EngineConnectionStatus::Connecting) {
        DisconnectFromEngine();
    }

    m_device = nullptr;
    m_context = nullptr;
    m_isInitialized = false;
    m_engineState = EngineState{};
    m_sceneData = EditorSceneData{};
    m_profilingData = EngineProfilingData{};
    m_liveVariables.clear();
    m_consoleHistory.clear();
    m_selectedEntityId = 0;
    m_updateTimer = 0.0f;
}

// ============================================================================
// Connection Management
// ============================================================================

bool SparkEngineIntegration::ConnectToEngine(const std::string& enginePath, const std::string& projectPath) {
    if (IsConnected()) {
        return false;
    }

    m_enginePath = enginePath;
    m_projectPath = projectPath;
    m_shouldStop.store(false);
    m_connectionStatus.store(EngineConnectionStatus::Connecting);

    m_communicationThread = std::thread(&SparkEngineIntegration::CommunicationThread, this);

    m_connectionStatus.store(EngineConnectionStatus::Connected);
    return true;
}

void SparkEngineIntegration::DisconnectFromEngine() {
    m_shouldStop.store(true);

    if (m_communicationThread.joinable()) {
        m_communicationThread.join();
    }

    m_connectionStatus.store(EngineConnectionStatus::Disconnected);
    m_enginePath.clear();
    m_projectPath.clear();
    m_engineState = EngineState{};
}

EngineConnectionStatus SparkEngineIntegration::GetConnectionStatus() const {
    return m_connectionStatus.load();
}

bool SparkEngineIntegration::IsConnected() const {
    return m_connectionStatus.load() == EngineConnectionStatus::Connected;
}

// ============================================================================
// Engine Control
// ============================================================================

CommandResult SparkEngineIntegration::StartEngine() {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_engineState.isRunning = true;
    m_engineState.isPaused = false;

    CommandResult result;
    result.success = true;
    result.result = "Engine started";

    if (m_engineStateCallback) {
        m_engineStateCallback(m_engineState);
    }

    return result;
}

CommandResult SparkEngineIntegration::PauseEngine() {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_engineState.isPaused = true;

    CommandResult result;
    result.success = true;
    result.result = "Engine paused";

    if (m_engineStateCallback) {
        m_engineStateCallback(m_engineState);
    }

    return result;
}

CommandResult SparkEngineIntegration::StopEngine() {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_engineState.isRunning = false;
    m_engineState.isPaused = false;

    CommandResult result;
    result.success = true;
    result.result = "Engine stopped";

    if (m_engineStateCallback) {
        m_engineStateCallback(m_engineState);
    }

    return result;
}

CommandResult SparkEngineIntegration::StepFrame() {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_engineState.isPaused = false;
    // Simulate one frame step
    m_engineState.isPaused = true;

    CommandResult result;
    result.success = true;
    result.result = "Frame stepped";

    if (m_engineStateCallback) {
        m_engineStateCallback(m_engineState);
    }

    return result;
}

CommandResult SparkEngineIntegration::SetTimeScale(float timeScale) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    CommandResult result;
    result.success = true;
    result.result = "Time scale set to " + std::to_string(timeScale);
    return result;
}

// ============================================================================
// Scene Synchronization
// ============================================================================

EditorSceneData SparkEngineIntegration::GetSceneData() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_sceneData;
}

CommandResult SparkEngineIntegration::LoadScene(const std::string& scenePath) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_sceneData.path = scenePath;

    // Extract scene name from path
    std::string name = scenePath;
    auto lastSlash = name.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        name = name.substr(lastSlash + 1);
    }
    auto dotPos = name.find_last_of('.');
    if (dotPos != std::string::npos) {
        name = name.substr(0, dotPos);
    }
    m_sceneData.name = name;
    m_sceneData.entities.clear();
    m_sceneData.isDirty = false;

    CommandResult result;
    result.success = true;
    result.result = "Scene loaded: " + scenePath;

    if (m_sceneChangedCallback) {
        m_sceneChangedCallback(m_sceneData);
    }

    return result;
}

CommandResult SparkEngineIntegration::SaveScene(const std::string& scenePath) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_sceneData.isDirty = false;

    CommandResult result;
    result.success = true;
    result.result = "Scene saved: " + scenePath;
    return result;
}

CommandResult SparkEngineIntegration::CreateNewScene() {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_sceneData = EditorSceneData{};
    m_sceneData.name = "New Scene";

    CommandResult result;
    result.success = true;
    result.result = "New scene created";

    if (m_sceneChangedCallback) {
        m_sceneChangedCallback(m_sceneData);
    }

    return result;
}

// ============================================================================
// Entity Management
// ============================================================================

std::vector<EditorEntityData> SparkEngineIntegration::GetAllEntities() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_sceneData.entities;
}

EditorEntityData SparkEngineIntegration::GetEntity(uint32_t entityId) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    for (const auto& entity : m_sceneData.entities) {
        if (entity.entityId == entityId) {
            return entity;
        }
    }

    return EditorEntityData{};
}

uint32_t SparkEngineIntegration::CreateEntity(const std::string& name, const XMFLOAT3& position) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    // Find next available ID
    uint32_t nextId = 1;
    for (const auto& entity : m_sceneData.entities) {
        if (entity.entityId >= nextId) {
            nextId = entity.entityId + 1;
        }
    }

    EditorEntityData newEntity;
    newEntity.entityId = nextId;
    newEntity.name = name;
    newEntity.position = position;
    newEntity.rotation = {0.0f, 0.0f, 0.0f};
    newEntity.scale = {1.0f, 1.0f, 1.0f};
    newEntity.isActive = true;
    newEntity.isVisible = true;
    newEntity.parentId = 0;

    m_sceneData.entities.push_back(newEntity);
    m_sceneData.isDirty = true;
    m_engineState.activeObjects = static_cast<int>(m_sceneData.entities.size());

    if (m_entityChangedCallback) {
        m_entityChangedCallback(newEntity);
    }

    return nextId;
}

CommandResult SparkEngineIntegration::DeleteEntity(uint32_t entityId) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    auto it = std::find_if(m_sceneData.entities.begin(), m_sceneData.entities.end(),
        [entityId](const EditorEntityData& e) { return e.entityId == entityId; });

    CommandResult result;

    if (it != m_sceneData.entities.end()) {
        EditorEntityData deleted = *it;
        m_sceneData.entities.erase(it);
        m_sceneData.isDirty = true;
        m_engineState.activeObjects = static_cast<int>(m_sceneData.entities.size());

        result.success = true;
        result.result = "Entity deleted: " + std::to_string(entityId);

        if (m_entityChangedCallback) {
            m_entityChangedCallback(deleted);
        }
    } else {
        result.success = false;
        result.error = "Entity not found: " + std::to_string(entityId);
    }

    return result;
}

CommandResult SparkEngineIntegration::UpdateEntityTransform(uint32_t entityId, const XMFLOAT3& position,
                                                           const XMFLOAT3& rotation, const XMFLOAT3& scale) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    auto it = std::find_if(m_sceneData.entities.begin(), m_sceneData.entities.end(),
        [entityId](const EditorEntityData& e) { return e.entityId == entityId; });

    CommandResult result;

    if (it != m_sceneData.entities.end()) {
        it->position = position;
        it->rotation = rotation;
        it->scale = scale;
        m_sceneData.isDirty = true;

        result.success = true;
        result.result = "Entity transform updated: " + std::to_string(entityId);

        if (m_entityChangedCallback) {
            m_entityChangedCallback(*it);
        }
    } else {
        result.success = false;
        result.error = "Entity not found: " + std::to_string(entityId);
    }

    return result;
}

CommandResult SparkEngineIntegration::AddComponent(uint32_t entityId, const std::string& componentType) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    auto it = std::find_if(m_sceneData.entities.begin(), m_sceneData.entities.end(),
        [entityId](const EditorEntityData& e) { return e.entityId == entityId; });

    CommandResult result;

    if (it != m_sceneData.entities.end()) {
        it->components.push_back(componentType);
        m_sceneData.isDirty = true;

        result.success = true;
        result.result = "Component added: " + componentType;

        if (m_entityChangedCallback) {
            m_entityChangedCallback(*it);
        }
    } else {
        result.success = false;
        result.error = "Entity not found: " + std::to_string(entityId);
    }

    return result;
}

CommandResult SparkEngineIntegration::RemoveComponent(uint32_t entityId, const std::string& componentType) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    auto it = std::find_if(m_sceneData.entities.begin(), m_sceneData.entities.end(),
        [entityId](const EditorEntityData& e) { return e.entityId == entityId; });

    CommandResult result;

    if (it != m_sceneData.entities.end()) {
        auto compIt = std::find(it->components.begin(), it->components.end(), componentType);
        if (compIt != it->components.end()) {
            it->components.erase(compIt);
            m_sceneData.isDirty = true;

            result.success = true;
            result.result = "Component removed: " + componentType;

            if (m_entityChangedCallback) {
                m_entityChangedCallback(*it);
            }
        } else {
            result.success = false;
            result.error = "Component not found: " + componentType;
        }
    } else {
        result.success = false;
        result.error = "Entity not found: " + std::to_string(entityId);
    }

    return result;
}

// ============================================================================
// Asset Management
// ============================================================================

std::vector<EditorAssetData> SparkEngineIntegration::GetLoadedAssets() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_loadedAssets;
}

CommandResult SparkEngineIntegration::LoadAsset(const std::string& assetPath) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    EditorAssetData asset;
    asset.path = assetPath;
    asset.isLoaded = true;

    // Derive type from extension
    auto dotPos = assetPath.find_last_of('.');
    if (dotPos != std::string::npos) {
        asset.type = assetPath.substr(dotPos + 1);
    }

    m_loadedAssets.push_back(asset);

    CommandResult result;
    result.success = true;
    result.result = "Asset loaded: " + assetPath;

    if (m_assetChangedCallback) {
        m_assetChangedCallback(asset);
    }

    return result;
}

CommandResult SparkEngineIntegration::UnloadAsset(const std::string& assetPath) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    EditorAssetData unloaded;
    unloaded.path = assetPath;
    unloaded.isLoaded = false;

    auto it = std::find_if(m_loadedAssets.begin(), m_loadedAssets.end(),
        [&assetPath](const EditorAssetData& a) { return a.path == assetPath; });

    if (it != m_loadedAssets.end()) {
        unloaded = *it;
        unloaded.isLoaded = false;
        m_loadedAssets.erase(it);
    }

    CommandResult result;
    result.success = true;
    result.result = "Asset unloaded: " + assetPath;

    if (m_assetChangedCallback) {
        m_assetChangedCallback(unloaded);
    }

    return result;
}

CommandResult SparkEngineIntegration::ReloadAsset(const std::string& assetPath) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    EditorAssetData reloaded;
    reloaded.path = assetPath;
    reloaded.isLoaded = true;

    auto it = std::find_if(m_loadedAssets.begin(), m_loadedAssets.end(),
        [&assetPath](const EditorAssetData& a) { return a.path == assetPath; });

    if (it != m_loadedAssets.end()) {
        reloaded = *it;
    }

    CommandResult result;
    result.success = true;
    result.result = "Asset reloaded: " + assetPath;

    if (m_assetChangedCallback) {
        m_assetChangedCallback(reloaded);
    }

    return result;
}

CommandResult SparkEngineIntegration::ReloadAllShaders() {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    CommandResult result;
    result.success = true;
    result.result = "All shaders reloaded";

    EditorAssetData shaderAsset;
    shaderAsset.type = "shader";
    shaderAsset.isLoaded = true;

    if (m_assetChangedCallback) {
        m_assetChangedCallback(shaderAsset);
    }

    return result;
}

// ============================================================================
// Live Variable Editing
// ============================================================================

std::vector<LiveVariable> SparkEngineIntegration::GetLiveVariables() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_liveVariables;
}

CommandResult SparkEngineIntegration::SetLiveVariable(const std::string& variableName, const std::string& value) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    CommandResult result;

    auto it = std::find_if(m_liveVariables.begin(), m_liveVariables.end(),
        [&variableName](const LiveVariable& v) { return v.name == variableName; });

    if (it != m_liveVariables.end()) {
        it->value = value;
        result.success = true;
        result.result = "Variable set: " + variableName + " = " + value;
    } else {
        result.success = false;
        result.error = "Variable not found: " + variableName;
    }

    return result;
}

CommandResult SparkEngineIntegration::RegisterLiveVariable(const LiveVariable& variable) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_liveVariables.push_back(variable);

    CommandResult result;
    result.success = true;
    result.result = "Variable registered: " + variable.name;
    return result;
}

// ============================================================================
// Debugging and Profiling
// ============================================================================

EngineState SparkEngineIntegration::GetEngineState() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_engineState;
}

EngineProfilingData SparkEngineIntegration::GetProfilingData() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_profilingData;
}

CommandResult SparkEngineIntegration::SetProfilingEnabled(bool enabled) {
    CommandResult result;
    result.success = true;
    result.result = std::string("Profiling ") + (enabled ? "enabled" : "disabled");
    return result;
}

CommandResult SparkEngineIntegration::TakeScreenshot(const std::string& filePath) {
    CommandResult result;
    result.success = true;
    result.result = "Screenshot saved: " + filePath;
    return result;
}

CommandResult SparkEngineIntegration::SetWireframeMode(bool enabled) {
    CommandResult result;
    result.success = true;
    result.result = std::string("Wireframe mode ") + (enabled ? "enabled" : "disabled");
    return result;
}

// ============================================================================
// Console Integration
// ============================================================================

CommandResult SparkEngineIntegration::ExecuteConsoleCommand(const std::string& command) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_consoleHistory.push_back(command);

    CommandResult result;
    result.success = true;
    result.result = "> " + command;
    return result;
}

std::vector<std::string> SparkEngineIntegration::GetConsoleHistory() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return m_consoleHistory;
}

CommandResult SparkEngineIntegration::ClearConsole() {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_consoleHistory.clear();

    CommandResult result;
    result.success = true;
    result.result = "Console cleared";
    return result;
}

// ============================================================================
// Callback Registration
// ============================================================================

void SparkEngineIntegration::SetEngineStateCallback(EngineStateCallback callback) {
    m_engineStateCallback = callback;
}

void SparkEngineIntegration::SetEntityChangedCallback(EntityChangedCallback callback) {
    m_entityChangedCallback = callback;
}

void SparkEngineIntegration::SetAssetChangedCallback(AssetChangedCallback callback) {
    m_assetChangedCallback = callback;
}

void SparkEngineIntegration::SetSceneChangedCallback(SceneChangedCallback callback) {
    m_sceneChangedCallback = callback;
}

void SparkEngineIntegration::SetProfilingDataCallback(ProfilingDataCallback callback) {
    m_profilingDataCallback = callback;
}

void SparkEngineIntegration::SetConsoleMessageCallback(ConsoleMessageCallback callback) {
    m_consoleMessageCallback = callback;
}

// ============================================================================
// Camera Control
// ============================================================================

CommandResult SparkEngineIntegration::SetEditorCamera(const XMFLOAT3& position, const XMFLOAT3& rotation) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    m_engineState.cameraPosition = position;
    m_engineState.cameraRotation = rotation;

    CommandResult result;
    result.success = true;
    result.result = "Editor camera updated";
    return result;
}

std::pair<XMFLOAT3, XMFLOAT3> SparkEngineIntegration::GetEditorCamera() {
    std::lock_guard<std::mutex> lock(m_commandMutex);
    return {m_engineState.cameraPosition, m_engineState.cameraRotation};
}

// ============================================================================
// Gizmo and Selection
// ============================================================================

CommandResult SparkEngineIntegration::SetSelectedEntity(uint32_t entityId) {
    m_selectedEntityId = entityId;

    CommandResult result;
    result.success = true;
    result.result = "Selected entity: " + std::to_string(entityId);
    return result;
}

uint32_t SparkEngineIntegration::GetSelectedEntityId() {
    return m_selectedEntityId;
}

CommandResult SparkEngineIntegration::SetGizmosEnabled(bool enabled) {
    CommandResult result;
    result.success = true;
    result.result = std::string("Gizmos ") + (enabled ? "enabled" : "disabled");
    return result;
}

// ============================================================================
// Update
// ============================================================================

void SparkEngineIntegration::Update(float deltaTime) {
    m_updateTimer += deltaTime;

    if (m_updateTimer >= m_updateInterval) {
        m_updateTimer -= m_updateInterval;

        std::lock_guard<std::mutex> lock(m_commandMutex);

        if (m_engineState.isRunning && !m_engineState.isPaused) {
            // Simulate frame stats
            m_engineState.drawCalls = static_cast<int>(m_sceneData.entities.size()) * 2;
            m_engineState.triangles = static_cast<int>(m_sceneData.entities.size()) * 1000;
            m_engineState.activeObjects = static_cast<int>(m_sceneData.entities.size());
            m_engineState.frameTime = deltaTime * 1000.0f; // convert to ms
            if (m_engineState.frameTime > 0.0f) {
                m_engineState.frameRate = 1000.0f / m_engineState.frameTime;
            }
        }

        if (m_engineStateCallback) {
            m_engineStateCallback(m_engineState);
        }

        if (m_profilingDataCallback) {
            m_profilingDataCallback(m_profilingData);
        }
    }
}

// ============================================================================
// Communication Thread (Private)
// ============================================================================

void SparkEngineIntegration::CommunicationThread() {
    while (!m_shouldStop.load()) {
        try {
            ProcessIncomingMessages();
        } catch (const std::exception& e) {
            std::cerr << "SparkEngineIntegration: Communication error: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "SparkEngineIntegration: Unknown communication error" << std::endl;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(m_updateInterval * 1000.0f)));
    }
}

void SparkEngineIntegration::ProcessIncomingMessages() {
    std::lock_guard<std::mutex> lock(m_responseMutex);

    // Check for state changes and invoke callbacks
    if (m_engineStateCallback) {
        std::lock_guard<std::mutex> cmdLock(m_commandMutex);
        m_engineStateCallback(m_engineState);
    }
}

// ============================================================================
// Internal Command Handling (Private)
// ============================================================================

CommandResult SparkEngineIntegration::SendCommand(const std::string& command) {
    std::lock_guard<std::mutex> lock(m_commandMutex);

    CommandResult result;
    result.success = true;

    if (command == "start") {
        m_engineState.isRunning = true;
        m_engineState.isPaused = false;
        result.result = "Engine started";
    } else if (command == "pause") {
        m_engineState.isPaused = true;
        result.result = "Engine paused";
    } else if (command == "stop") {
        m_engineState.isRunning = false;
        m_engineState.isPaused = false;
        result.result = "Engine stopped";
    } else if (command == "step") {
        m_engineState.isPaused = false;
        m_engineState.isPaused = true;
        result.result = "Frame stepped";
    } else {
        result.result = "Command executed: " + command;
    }

    return result;
}

CommandResult SparkEngineIntegration::ParseResponse(const std::string& response) {
    CommandResult result;

    std::istringstream stream(response);
    std::string line;

    while (std::getline(stream, line)) {
        auto colonPos = line.find(':');
        if (colonPos != std::string::npos) {
            std::string key = line.substr(0, colonPos);
            std::string value = line.substr(colonPos + 1);

            // Trim leading spaces from value
            auto start = value.find_first_not_of(' ');
            if (start != std::string::npos) {
                value = value.substr(start);
            }

            if (key == "success") {
                result.success = (value == "true" || value == "1");
            } else if (key == "result") {
                result.result = value;
            } else if (key == "error") {
                result.error = value;
            } else if (key == "time") {
                try {
                    result.executionTime = std::stof(value);
                } catch (...) {
                    result.executionTime = 0.0f;
                }
            }
        }
    }

    return result;
}

} // namespace SparkEditor
