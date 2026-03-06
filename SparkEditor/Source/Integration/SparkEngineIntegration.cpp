/**
 * @file SparkEngineIntegration.cpp
 * @brief Stub implementation of SparkEngineIntegration
 */

#include "SparkEngineIntegration.h"

namespace SparkEditor {

SparkEngineIntegration::SparkEngineIntegration() {
}

SparkEngineIntegration::~SparkEngineIntegration() {
}

bool SparkEngineIntegration::Initialize(ID3D11Device* /*device*/, ID3D11DeviceContext* /*context*/) {
    return false;
}

void SparkEngineIntegration::Update(float /*deltaTime*/) {
}

void SparkEngineIntegration::Shutdown() {
}

bool SparkEngineIntegration::ConnectToEngine(const std::string& /*enginePath*/, const std::string& /*projectPath*/) {
    return false;
}

void SparkEngineIntegration::DisconnectFromEngine() {
}

EngineConnectionStatus SparkEngineIntegration::GetConnectionStatus() const {
    return m_connectionStatus.load();
}

bool SparkEngineIntegration::IsConnected() const {
    return false;
}

CommandResult SparkEngineIntegration::StartEngine() {
    return {};
}

CommandResult SparkEngineIntegration::PauseEngine() {
    return {};
}

CommandResult SparkEngineIntegration::StopEngine() {
    return {};
}

CommandResult SparkEngineIntegration::StepFrame() {
    return {};
}

CommandResult SparkEngineIntegration::SetTimeScale(float /*timeScale*/) {
    return {};
}

EditorSceneData SparkEngineIntegration::GetSceneData() {
    return {};
}

CommandResult SparkEngineIntegration::LoadScene(const std::string& /*scenePath*/) {
    return {};
}

CommandResult SparkEngineIntegration::SaveScene(const std::string& /*scenePath*/) {
    return {};
}

CommandResult SparkEngineIntegration::CreateNewScene() {
    return {};
}

std::vector<EditorEntityData> SparkEngineIntegration::GetAllEntities() {
    return {};
}

EditorEntityData SparkEngineIntegration::GetEntity(uint32_t /*entityId*/) {
    return {};
}

uint32_t SparkEngineIntegration::CreateEntity(const std::string& /*name*/, const XMFLOAT3& /*position*/) {
    return 0;
}

CommandResult SparkEngineIntegration::DeleteEntity(uint32_t /*entityId*/) {
    return {};
}

CommandResult SparkEngineIntegration::UpdateEntityTransform(uint32_t /*entityId*/, const XMFLOAT3& /*position*/,
                                                           const XMFLOAT3& /*rotation*/, const XMFLOAT3& /*scale*/) {
    return {};
}

CommandResult SparkEngineIntegration::AddComponent(uint32_t /*entityId*/, const std::string& /*componentType*/) {
    return {};
}

CommandResult SparkEngineIntegration::RemoveComponent(uint32_t /*entityId*/, const std::string& /*componentType*/) {
    return {};
}

std::vector<EditorAssetData> SparkEngineIntegration::GetLoadedAssets() {
    return {};
}

CommandResult SparkEngineIntegration::LoadAsset(const std::string& /*assetPath*/) {
    return {};
}

CommandResult SparkEngineIntegration::UnloadAsset(const std::string& /*assetPath*/) {
    return {};
}

CommandResult SparkEngineIntegration::ReloadAsset(const std::string& /*assetPath*/) {
    return {};
}

CommandResult SparkEngineIntegration::ReloadAllShaders() {
    return {};
}

std::vector<LiveVariable> SparkEngineIntegration::GetLiveVariables() {
    return {};
}

CommandResult SparkEngineIntegration::SetLiveVariable(const std::string& /*variableName*/, const std::string& /*value*/) {
    return {};
}

CommandResult SparkEngineIntegration::RegisterLiveVariable(const LiveVariable& /*variable*/) {
    return {};
}

EngineState SparkEngineIntegration::GetEngineState() {
    return {};
}

EngineProfilingData SparkEngineIntegration::GetProfilingData() {
    return {};
}

CommandResult SparkEngineIntegration::SetProfilingEnabled(bool /*enabled*/) {
    return {};
}

CommandResult SparkEngineIntegration::TakeScreenshot(const std::string& /*filePath*/) {
    return {};
}

CommandResult SparkEngineIntegration::SetWireframeMode(bool /*enabled*/) {
    return {};
}

CommandResult SparkEngineIntegration::ExecuteConsoleCommand(const std::string& /*command*/) {
    return {};
}

std::vector<std::string> SparkEngineIntegration::GetConsoleHistory() {
    return {};
}

CommandResult SparkEngineIntegration::ClearConsole() {
    return {};
}

void SparkEngineIntegration::SetEngineStateCallback(EngineStateCallback /*callback*/) {
}

void SparkEngineIntegration::SetEntityChangedCallback(EntityChangedCallback /*callback*/) {
}

void SparkEngineIntegration::SetAssetChangedCallback(AssetChangedCallback /*callback*/) {
}

void SparkEngineIntegration::SetSceneChangedCallback(SceneChangedCallback /*callback*/) {
}

void SparkEngineIntegration::SetProfilingDataCallback(ProfilingDataCallback /*callback*/) {
}

void SparkEngineIntegration::SetConsoleMessageCallback(ConsoleMessageCallback /*callback*/) {
}

CommandResult SparkEngineIntegration::SetEditorCamera(const XMFLOAT3& /*position*/, const XMFLOAT3& /*rotation*/) {
    return {};
}

std::pair<XMFLOAT3, XMFLOAT3> SparkEngineIntegration::GetEditorCamera() {
    return {{0, 0, 0}, {0, 0, 0}};
}

CommandResult SparkEngineIntegration::SetSelectedEntity(uint32_t /*entityId*/) {
    return {};
}

uint32_t SparkEngineIntegration::GetSelectedEntityId() {
    return 0;
}

CommandResult SparkEngineIntegration::SetGizmosEnabled(bool /*enabled*/) {
    return {};
}

void SparkEngineIntegration::CommunicationThread() {
}

void SparkEngineIntegration::ProcessIncomingMessages() {
}

CommandResult SparkEngineIntegration::SendCommand(const std::string& /*command*/) {
    return {};
}

CommandResult SparkEngineIntegration::ParseResponse(const std::string& /*response*/) {
    return {};
}

} // namespace SparkEditor
