#include "LevelStreamingSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <chrono>

using namespace DirectX;
namespace SparkEditor
{

    // --- WorldTile ---

    bool WorldTile::ContainsPoint(const XMFLOAT3& point) const
    {
        float halfX = worldSize.x * 0.5f;
        float halfZ = worldSize.z * 0.5f;
        return point.x >= worldPosition.x - halfX && point.x <= worldPosition.x + halfX &&
               point.z >= worldPosition.z - halfZ && point.z <= worldPosition.z + halfZ;
    }

    float WorldTile::GetDistanceToCenter(const XMFLOAT3& point) const
    {
        float dx = point.x - worldPosition.x;
        float dy = point.y - worldPosition.y;
        float dz = point.z - worldPosition.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float WorldTile::GetDistanceToBounds(const XMFLOAT3& point) const
    {
        float halfX = worldSize.x * 0.5f;
        float halfY = worldSize.y * 0.5f;
        float halfZ = worldSize.z * 0.5f;

        float dx = std::max(0.0f, std::abs(point.x - worldPosition.x) - halfX);
        float dy = std::max(0.0f, std::abs(point.y - worldPosition.y) - halfY);
        float dz = std::max(0.0f, std::abs(point.z - worldPosition.z) - halfZ);

        float outsideDist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (outsideDist > 0.0f)
            return outsideDist;

        // Inside: return negative closest distance to face
        float mx = halfX - std::abs(point.x - worldPosition.x);
        float my = halfY - std::abs(point.y - worldPosition.y);
        float mz = halfZ - std::abs(point.z - worldPosition.z);
        return -std::min({mx, my, mz});
    }

    LODLevel WorldTile::CalculateLOD(float distance) const
    {
        for (size_t i = 0; i < lodDistances.size(); ++i)
        {
            if (distance < lodDistances[i])
                return static_cast<LODLevel>(i);
        }
        return LODLevel::LOD_4;
    }

    // --- StreamingVolume ---

    bool StreamingVolume::ContainsPoint(const XMFLOAT3& point) const
    {
        float halfX = size.x * 0.5f;
        float halfY = size.y * 0.5f;
        float halfZ = size.z * 0.5f;
        return point.x >= center.x - halfX && point.x <= center.x + halfX && point.y >= center.y - halfY &&
               point.y <= center.y + halfY && point.z >= center.z - halfZ && point.z <= center.z + halfZ;
    }

    // --- StreamingViewer ---

    XMFLOAT3 StreamingViewer::GetPredictedPosition(float predictionTime) const
    {
        return {position.x + velocity.x * predictionTime, position.y + velocity.y * predictionTime,
                position.z + velocity.z * predictionTime};
    }

    bool StreamingViewer::IsInViewFrustum(const XMFLOAT3& pos, float radius) const
    {
        // Simplified cone-based frustum check
        float dx = pos.x - position.x;
        float dy = pos.y - position.y;
        float dz = pos.z - position.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < 1e-6f)
            return true;

        // Dot product with forward vector
        float dot = (dx * forward.x + dy * forward.y + dz * forward.z) / dist;
        float halfAngleRad = MathUtils::DegreesToRadians(fieldOfView * 0.5f);
        float cosHalf = std::cos(halfAngleRad);

        // Expand cone by object radius
        float sinExpand = radius / dist;
        float adjustedCos = cosHalf - sinExpand;
        return dot >= adjustedCos;
    }

    // --- LevelStreamingSystem ---

    LevelStreamingSystem::LevelStreamingSystem() : EditorPanel("Level Streaming", "level_streaming") {}

    LevelStreamingSystem::~LevelStreamingSystem()
    {
        m_shouldStopLoading.store(true);
        m_loadingCondition.notify_all();
        for (auto& thread : m_loadingThreads)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    bool LevelStreamingSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        m_lastStatsUpdate = std::chrono::steady_clock::now();
        return true;
    }

    void LevelStreamingSystem::Update(float /*deltaTime*/)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Scene);
        if (m_streamingPaused)
            return;

        if (m_automaticStreaming)
            UpdateAutomaticStreaming();

        ProcessLoadingQueue();
        ProcessUnloadingQueue();
        UpdateLODSystem();
        UpdateMemoryManagement();
        UpdateStatistics();
    }

    void LevelStreamingSystem::Render()
    {
        if (!ImGui::Begin(GetName().c_str(), &m_isVisible))
        {
            ImGui::End();
            return;
        }

        // Toolbar
        if (ImGui::Button("New World"))
        {
            CreateNewWorld("New World", m_worldSettings);
        }
        ImGui::SameLine();
        bool streamingPaused = m_streamingPaused;
        if (ImGui::Checkbox("Pause Streaming", &streamingPaused))
        {
            m_streamingPaused = streamingPaused;
        }

        ImGui::Separator();
        ImGui::Text("World: %s  |  Tiles: %d", m_worldName.c_str(), static_cast<int>(m_tiles.size()));
        ImGui::Separator();

        // Tabs for sub-panels
        if (ImGui::BeginTabBar("LevelStreamingTabs"))
        {
            if (ImGui::BeginTabItem("World Overview"))
            {
                RenderWorldOverview();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tiles"))
            {
                RenderTileList();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Volumes"))
            {
                RenderStreamingVolumes();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings"))
            {
                RenderWorldSettings();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Statistics"))
            {
                RenderStreamingStatistics();
                ImGui::EndTabItem();
            }
            if (m_showDebugInfo && ImGui::BeginTabItem("Debug"))
            {
                RenderDebugInfo();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    void LevelStreamingSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "LevelStreamingSystem shutting down (%zu tiles)", m_tiles.size());
        m_shouldStopLoading.store(true);
        m_loadingCondition.notify_all();
        for (auto& thread : m_loadingThreads)
        {
            if (thread.joinable())
                thread.join();
        }
        m_loadingThreads.clear();
        m_tiles.clear();
        m_streamingVolumes.clear();
    }

    bool LevelStreamingSystem::HandleEvent(const std::string& /*eventType*/, void* /*eventData*/)
    {
        return false;
    }

    void LevelStreamingSystem::CreateNewWorld(const std::string& name, const WorldCompositionSettings& settings)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Creating new world: '%s'", name.c_str());
        m_worldName = name;
        m_worldSettings = settings;
        m_tiles.clear();
        m_streamingVolumes.clear();
        m_selectedTile.clear();
        m_statistics = {};
        SetModified(true);
    }

    bool LevelStreamingSystem::LoadWorld(const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading world from: %s", filePath.c_str());
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to open world file: %s", filePath.c_str());
            return false;
        }

        // Read world name length + name
        uint32_t nameLen = 0;
        file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (nameLen > 4096)
            return false;
        m_worldName.resize(nameLen);
        file.read(m_worldName.data(), nameLen);

        // Read tile count
        uint32_t tileCount = 0;
        file.read(reinterpret_cast<char*>(&tileCount), sizeof(tileCount));
        if (tileCount > 100000)
            return false;

        m_tiles.clear();
        m_tiles.reserve(tileCount);
        for (uint32_t i = 0; i < tileCount; ++i)
        {
            WorldTile tile;
            // Read tile name
            uint32_t tileNameLen = 0;
            file.read(reinterpret_cast<char*>(&tileNameLen), sizeof(tileNameLen));
            if (tileNameLen > 4096)
                return false;
            tile.name.resize(tileNameLen);
            file.read(tile.name.data(), tileNameLen);

            // Read tile file path
            uint32_t pathLen = 0;
            file.read(reinterpret_cast<char*>(&pathLen), sizeof(pathLen));
            if (pathLen > 4096)
                return false;
            tile.filePath.resize(pathLen);
            file.read(tile.filePath.data(), pathLen);

            // Read transform data
            file.read(reinterpret_cast<char*>(&tile.worldPosition), sizeof(XMFLOAT3));
            file.read(reinterpret_cast<char*>(&tile.worldSize), sizeof(XMFLOAT3));
            file.read(reinterpret_cast<char*>(&tile.tileCoordinates), sizeof(XMFLOAT2));
            file.read(reinterpret_cast<char*>(&tile.streamingDistance), sizeof(float));
            file.read(reinterpret_cast<char*>(&tile.unloadingDistance), sizeof(float));
            file.read(reinterpret_cast<char*>(&tile.priority), sizeof(int));
            file.read(reinterpret_cast<char*>(&tile.alwaysLoaded), sizeof(bool));

            tile.state = StreamingState::UNLOADED;
            m_tiles.push_back(std::move(tile));
        }

        // Read volume count
        uint32_t volumeCount = 0;
        file.read(reinterpret_cast<char*>(&volumeCount), sizeof(volumeCount));
        if (volumeCount > 10000)
            return false;

        m_streamingVolumes.clear();
        m_streamingVolumes.reserve(volumeCount);
        for (uint32_t i = 0; i < volumeCount; ++i)
        {
            StreamingVolume vol;
            uint32_t volNameLen = 0;
            file.read(reinterpret_cast<char*>(&volNameLen), sizeof(volNameLen));
            if (volNameLen > 4096)
                return false;
            vol.name.resize(volNameLen);
            file.read(vol.name.data(), volNameLen);
            file.read(reinterpret_cast<char*>(&vol.center), sizeof(XMFLOAT3));
            file.read(reinterpret_cast<char*>(&vol.size), sizeof(XMFLOAT3));
            m_streamingVolumes.push_back(std::move(vol));
        }

        SetModified(false);
        return file.good();
    }

    bool LevelStreamingSystem::SaveWorld(const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving world '%s' to: %s", m_worldName.c_str(), filePath.c_str());
        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to save world to: %s", filePath.c_str());
            return false;
        }

        // Write world name
        auto nameLen = static_cast<uint32_t>(m_worldName.size());
        file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        file.write(m_worldName.data(), nameLen);

        // Write tiles
        auto tileCount = static_cast<uint32_t>(m_tiles.size());
        file.write(reinterpret_cast<const char*>(&tileCount), sizeof(tileCount));
        for (const auto& tile : m_tiles)
        {
            auto tileNameLen = static_cast<uint32_t>(tile.name.size());
            file.write(reinterpret_cast<const char*>(&tileNameLen), sizeof(tileNameLen));
            file.write(tile.name.data(), tileNameLen);

            auto pathLen = static_cast<uint32_t>(tile.filePath.size());
            file.write(reinterpret_cast<const char*>(&pathLen), sizeof(pathLen));
            file.write(tile.filePath.data(), pathLen);

            file.write(reinterpret_cast<const char*>(&tile.worldPosition), sizeof(XMFLOAT3));
            file.write(reinterpret_cast<const char*>(&tile.worldSize), sizeof(XMFLOAT3));
            file.write(reinterpret_cast<const char*>(&tile.tileCoordinates), sizeof(XMFLOAT2));
            file.write(reinterpret_cast<const char*>(&tile.streamingDistance), sizeof(float));
            file.write(reinterpret_cast<const char*>(&tile.unloadingDistance), sizeof(float));
            file.write(reinterpret_cast<const char*>(&tile.priority), sizeof(int));
            file.write(reinterpret_cast<const char*>(&tile.alwaysLoaded), sizeof(bool));
        }

        // Write volumes
        auto volumeCount = static_cast<uint32_t>(m_streamingVolumes.size());
        file.write(reinterpret_cast<const char*>(&volumeCount), sizeof(volumeCount));
        for (const auto& vol : m_streamingVolumes)
        {
            auto volNameLen = static_cast<uint32_t>(vol.name.size());
            file.write(reinterpret_cast<const char*>(&volNameLen), sizeof(volNameLen));
            file.write(vol.name.data(), volNameLen);
            file.write(reinterpret_cast<const char*>(&vol.center), sizeof(XMFLOAT3));
            file.write(reinterpret_cast<const char*>(&vol.size), sizeof(XMFLOAT3));
        }

        SetModified(false);
        return file.good();
    }

    bool LevelStreamingSystem::AddTile(WorldTile tile)
    {
        // Check for duplicate name
        for (const auto& t : m_tiles)
        {
            if (t.name == tile.name)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "Duplicate tile name: '%s'", tile.name.c_str());
                return false;
            }
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Adding tile '%s' at position (%.1f, %.1f, %.1f)", tile.name.c_str(),
                       tile.worldPosition.x, tile.worldPosition.y, tile.worldPosition.z);
        m_tiles.push_back(std::move(tile));
        m_tiles.back().state = StreamingState::UNLOADED;
        SetModified(true);
        return true;
    }

    bool LevelStreamingSystem::RemoveTile(const std::string& tileName)
    {
        auto it = std::find_if(m_tiles.begin(), m_tiles.end(), [&](const WorldTile& t) { return t.name == tileName; });
        if (it == m_tiles.end())
            return false;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Removing tile: '%s'", tileName.c_str());

        if (it->state == StreamingState::LOADED)
            UnloadTileSync(tileName);

        if (m_selectedTile == tileName)
            m_selectedTile.clear();

        m_tiles.erase(it);
        SetModified(true);
        return true;
    }

    WorldTile* LevelStreamingSystem::GetTile(const std::string& tileName)
    {
        for (auto& tile : m_tiles)
        {
            if (tile.name == tileName)
                return &tile;
        }
        return nullptr;
    }

    WorldTile* LevelStreamingSystem::GetTileAtPosition(const XMFLOAT3& worldPosition)
    {
        for (auto& tile : m_tiles)
        {
            if (tile.ContainsPoint(worldPosition))
                return &tile;
        }
        return nullptr;
    }

    void LevelStreamingSystem::AddStreamingVolume(const StreamingVolume& volume)
    {
        m_streamingVolumes.push_back(volume);
        SetModified(true);
    }

    bool LevelStreamingSystem::RemoveStreamingVolume(const std::string& volumeName)
    {
        auto it = std::find_if(m_streamingVolumes.begin(), m_streamingVolumes.end(),
                               [&](const StreamingVolume& v) { return v.name == volumeName; });
        if (it == m_streamingVolumes.end())
            return false;
        m_streamingVolumes.erase(it);
        SetModified(true);
        return true;
    }

    void LevelStreamingSystem::SetStreamingViewer(const StreamingViewer& viewer)
    {
        m_streamingViewer = viewer;
    }

    bool LevelStreamingSystem::RequestTileLoad(const std::string& tileName, int priority, bool blockOnLoad)
    {
        WorldTile* tile = GetTile(tileName);
        if (!tile || tile->state == StreamingState::LOADED || tile->state == StreamingState::LOADING)
            return false;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Requesting tile load: '%s' (priority=%d, blocking=%s)",
                       tileName.c_str(), priority, blockOnLoad ? "true" : "false");

        if (blockOnLoad)
            return LoadTileSync(tileName);

        std::lock_guard<std::mutex> lock(m_queueMutex);
        LoadingRequest req;
        req.tileName = tileName;
        req.priority = priority;
        req.blockOnLoad = false;
        req.requestTime = std::chrono::steady_clock::now();
        m_loadingQueue.push(req);
        m_statistics.loadRequests++;
        return true;
    }

    bool LevelStreamingSystem::RequestTileUnload(const std::string& tileName, bool immediate)
    {
        WorldTile* tile = GetTile(tileName);
        if (!tile || tile->state != StreamingState::LOADED)
            return false;
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Requesting tile unload: '%s' (immediate=%s)", tileName.c_str(),
                       immediate ? "true" : "false");

        if (immediate)
            return ForceUnloadTile(tileName);

        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_unloadingQueue.push(tileName);
        m_statistics.unloadRequests++;
        return true;
    }

    bool LevelStreamingSystem::ForceLoadTile(const std::string& tileName)
    {
        return LoadTileSync(tileName);
    }

    bool LevelStreamingSystem::ForceUnloadTile(const std::string& tileName)
    {
        return UnloadTileSync(tileName);
    }

    std::vector<WorldTile*> LevelStreamingSystem::GetTilesWithinDistance(const XMFLOAT3& position, float distance)
    {
        std::vector<WorldTile*> result;
        for (auto& tile : m_tiles)
        {
            if (tile.GetDistanceToBounds(position) <= distance)
                result.push_back(&tile);
        }
        return result;
    }

    std::vector<WorldTile*> LevelStreamingSystem::GetVisibleTiles(const XMFLOAT3& position, const XMFLOAT3& forward,
                                                                  float fieldOfView)
    {
        StreamingViewer viewer;
        viewer.position = position;
        viewer.forward = forward;
        viewer.fieldOfView = fieldOfView;

        std::vector<WorldTile*> result;
        for (auto& tile : m_tiles)
        {
            float bsRadius = tile.boundingSphere.w;
            XMFLOAT3 bsCenter = {tile.boundingSphere.x, tile.boundingSphere.y, tile.boundingSphere.z};
            if (bsCenter.x == 0.0f && bsCenter.y == 0.0f && bsCenter.z == 0.0f)
                bsCenter = tile.worldPosition;
            if (bsRadius <= 0.0f)
                bsRadius = std::max(tile.worldSize.x, tile.worldSize.z) * 0.5f;

            if (viewer.IsInViewFrustum(bsCenter, bsRadius))
                result.push_back(&tile);
        }
        return result;
    }

    void LevelStreamingSystem::UpdateTileLOD(const std::string& tileName, float viewerDistance)
    {
        WorldTile* tile = GetTile(tileName);
        if (!tile)
            return;
        tile->currentLOD = tile->CalculateLOD(viewerDistance * m_worldSettings.lodBias);
    }

    StreamingStatistics LevelStreamingSystem::GetStreamingStatistics() const
    {
        return m_statistics;
    }

    void LevelStreamingSystem::SetWorldSettings(const WorldCompositionSettings& settings)
    {
        m_worldSettings = settings;
        SetModified(true);
    }

    int LevelStreamingSystem::GenerateTileGridFromHeightmap(const std::string& /*heightmapPath*/,
                                                            const XMFLOAT2& tileSize, const XMFLOAT2& worldSize)
    {
        int tilesX = std::max(1, static_cast<int>(std::ceil(worldSize.x / tileSize.x)));
        int tilesY = std::max(1, static_cast<int>(std::ceil(worldSize.y / tileSize.y)));
        int count = 0;

        for (int y = 0; y < tilesY; ++y)
        {
            for (int x = 0; x < tilesX; ++x)
            {
                WorldTile tile;
                tile.name = "Tile_" + std::to_string(x) + "_" + std::to_string(y);
                tile.tileCoordinates = {static_cast<float>(x), static_cast<float>(y)};
                tile.worldPosition = {x * tileSize.x + tileSize.x * 0.5f, 0.0f, y * tileSize.y + tileSize.y * 0.5f};
                tile.worldSize = {tileSize.x, 100.0f, tileSize.y};
                tile.streamingDistance = m_worldSettings.defaultStreamingDistance;
                tile.unloadingDistance = m_worldSettings.defaultUnloadingDistance;
                tile.streamingMethod = m_worldSettings.defaultStreamingMethod;
                if (AddTile(std::move(tile)))
                    ++count;
            }
        }
        return count;
    }

    int LevelStreamingSystem::OptimizeTileArrangement(size_t targetMemoryUsage)
    {
        // Sort tiles by priority (low priority first) for potential unloading
        std::vector<WorldTile*> loadedTiles;
        for (auto& tile : m_tiles)
        {
            if (tile.state == StreamingState::LOADED)
                loadedTiles.push_back(&tile);
        }

        std::sort(loadedTiles.begin(), loadedTiles.end(),
                  [](const WorldTile* a, const WorldTile* b) { return a->priority < b->priority; });

        int optimized = 0;
        size_t currentMemory = m_statistics.memoryUsage;
        for (auto* tile : loadedTiles)
        {
            if (currentMemory <= targetMemoryUsage)
                break;
            if (tile->alwaysLoaded)
                continue;
            currentMemory -= tile->memoryUsage;
            UnloadTileSync(tile->name);
            ++optimized;
        }
        return optimized;
    }

    bool LevelStreamingSystem::ValidateWorld(std::vector<std::string>& errors) const
    {
        bool valid = true;

        if (m_worldName.empty())
        {
            errors.push_back("World name is empty");
            valid = false;
        }

        // Check for duplicate tile names
        for (size_t i = 0; i < m_tiles.size(); ++i)
        {
            if (m_tiles[i].name.empty())
            {
                errors.push_back("Tile at index " + std::to_string(i) + " has empty name");
                valid = false;
            }
            for (size_t j = i + 1; j < m_tiles.size(); ++j)
            {
                if (m_tiles[i].name == m_tiles[j].name)
                {
                    errors.push_back("Duplicate tile name: " + m_tiles[i].name);
                    valid = false;
                }
            }
        }

        // Check streaming distances
        for (const auto& tile : m_tiles)
        {
            if (tile.streamingDistance >= tile.unloadingDistance)
            {
                errors.push_back("Tile '" + tile.name +
                                 "': streaming distance >= unloading distance (may cause thrashing)");
                valid = false;
            }
        }

        // Check for overlapping volumes with same tile references
        for (size_t i = 0; i < m_streamingVolumes.size(); ++i)
        {
            if (m_streamingVolumes[i].name.empty())
            {
                errors.push_back("Streaming volume at index " + std::to_string(i) + " has empty name");
                valid = false;
            }
        }

        return valid;
    }

    // --- Private render methods ---

    void LevelStreamingSystem::RenderWorldOverview()
    {
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 50.0f || canvasSize.y < 50.0f)
            return;

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(30, 30, 30, 255));

        // Draw grid
        float gridStep = 50.0f * m_overviewZoom;
        for (float x = 0; x < canvasSize.x; x += gridStep)
        {
            drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y), ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y),
                              IM_COL32(50, 50, 50, 255));
        }
        for (float y = 0; y < canvasSize.y; y += gridStep)
        {
            drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y), ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y),
                              IM_COL32(50, 50, 50, 255));
        }

        // Draw tiles
        float centerX = canvasPos.x + canvasSize.x * 0.5f + m_overviewOffset.x;
        float centerY = canvasPos.y + canvasSize.y * 0.5f + m_overviewOffset.y;
        float scale = m_overviewZoom * 0.05f; // world units to pixels

        for (const auto& tile : m_tiles)
        {
            float tileScreenX = centerX + tile.worldPosition.x * scale;
            float tileScreenY = centerY + tile.worldPosition.z * scale;
            float halfW = tile.worldSize.x * scale * 0.5f;
            float halfH = tile.worldSize.z * scale * 0.5f;

            ImU32 color;
            switch (tile.state)
            {
            case StreamingState::LOADED:
                color = IM_COL32(60, 180, 60, 200);
                break;
            case StreamingState::LOADING:
                color = IM_COL32(200, 200, 60, 200);
                break;
            case StreamingState::UNLOADING:
                color = IM_COL32(200, 120, 60, 200);
                break;
            case StreamingState::FAILED:
                color = IM_COL32(200, 60, 60, 200);
                break;
            default:
                color = IM_COL32(80, 80, 80, 200);
                break;
            }

            ImVec2 tileMin = {tileScreenX - halfW, tileScreenY - halfH};
            ImVec2 tileMax = {tileScreenX + halfW, tileScreenY + halfH};
            drawList->AddRectFilled(tileMin, tileMax, color);
            drawList->AddRect(tileMin, tileMax,
                              (tile.name == m_selectedTile) ? IM_COL32(255, 255, 0, 255)
                                                            : IM_COL32(150, 150, 150, 255));

            // Tile label
            if (halfW > 10.0f)
            {
                drawList->AddText(ImVec2(tileMin.x + 2, tileMin.y + 2), IM_COL32(255, 255, 255, 200),
                                  tile.name.c_str());
            }
        }

        // Draw viewer position
        float viewerX = centerX + m_streamingViewer.position.x * scale;
        float viewerY = centerY + m_streamingViewer.position.z * scale;
        drawList->AddCircleFilled(ImVec2(viewerX, viewerY), 5.0f, IM_COL32(255, 255, 0, 255));

        // Handle mouse interaction for pan/zoom
        ImGui::InvisibleButton("world_overview_canvas", canvasSize);
        if (ImGui::IsItemHovered())
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
                m_overviewZoom = std::clamp(m_overviewZoom + wheel * 0.1f, 0.1f, 10.0f);

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                m_overviewOffset.x += delta.x;
                m_overviewOffset.y += delta.y;
            }
        }
    }

    void LevelStreamingSystem::RenderTileList()
    {
        // Add tile button
        if (ImGui::Button("Add Tile"))
        {
            WorldTile newTile;
            newTile.name = "Tile_" + std::to_string(m_tiles.size());
            newTile.streamingDistance = m_worldSettings.defaultStreamingDistance;
            newTile.unloadingDistance = m_worldSettings.defaultUnloadingDistance;
            AddTile(std::move(newTile));
        }

        ImGui::Separator();

        // Tile list
        if (ImGui::BeginChild("TileList", ImVec2(0, 0), true))
        {
            for (size_t i = 0; i < m_tiles.size(); ++i)
            {
                auto& tile = m_tiles[i];
                bool isSelected = (tile.name == m_selectedTile);

                // State indicator color
                ImVec4 stateColor;
                const char* stateStr;
                switch (tile.state)
                {
                case StreamingState::LOADED:
                    stateColor = {0.2f, 0.8f, 0.2f, 1.0f};
                    stateStr = "Loaded";
                    break;
                case StreamingState::LOADING:
                    stateColor = {0.8f, 0.8f, 0.2f, 1.0f};
                    stateStr = "Loading";
                    break;
                case StreamingState::UNLOADING:
                    stateColor = {0.8f, 0.5f, 0.2f, 1.0f};
                    stateStr = "Unloading";
                    break;
                case StreamingState::FAILED:
                    stateColor = {0.8f, 0.2f, 0.2f, 1.0f};
                    stateStr = "Failed";
                    break;
                default:
                    stateColor = {0.5f, 0.5f, 0.5f, 1.0f};
                    stateStr = "Unloaded";
                    break;
                }

                ImGui::PushID(static_cast<int>(i));
                ImGui::TextColored(stateColor, "[%s]", stateStr);
                ImGui::SameLine();

                if (ImGui::Selectable(tile.name.c_str(), isSelected))
                    m_selectedTile = tile.name;

                if (isSelected && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    // Double-click to load/unload
                    if (tile.state == StreamingState::UNLOADED)
                        RequestTileLoad(tile.name, tile.priority);
                    else if (tile.state == StreamingState::LOADED)
                        RequestTileUnload(tile.name);
                }

                // Tile details on selection
                if (isSelected)
                {
                    ImGui::Indent();
                    ImGui::Text("Position: (%.0f, %.0f, %.0f)", tile.worldPosition.x, tile.worldPosition.y,
                                tile.worldPosition.z);
                    ImGui::Text("Size: (%.0f, %.0f, %.0f)", tile.worldSize.x, tile.worldSize.y, tile.worldSize.z);
                    ImGui::Text("LOD: %d  |  Priority: %d", static_cast<int>(tile.currentLOD), tile.priority);
                    ImGui::SliderFloat("Stream Dist", &tile.streamingDistance, 100.0f, 10000.0f);
                    ImGui::SliderFloat("Unload Dist", &tile.unloadingDistance, 100.0f, 15000.0f);
                    ImGui::Checkbox("Always Loaded", &tile.alwaysLoaded);

                    if (tile.state == StreamingState::UNLOADED && ImGui::Button("Load"))
                        RequestTileLoad(tile.name, tile.priority);
                    if (tile.state == StreamingState::LOADED)
                    {
                        ImGui::SameLine();
                        if (ImGui::Button("Unload"))
                            RequestTileUnload(tile.name);
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Remove"))
                    {
                        RemoveTile(tile.name);
                        ImGui::PopID();
                        ImGui::Unindent();
                        break;
                    }
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void LevelStreamingSystem::RenderStreamingVolumes()
    {
        if (ImGui::Button("Add Volume"))
        {
            StreamingVolume vol;
            vol.name = "Volume_" + std::to_string(m_streamingVolumes.size());
            AddStreamingVolume(vol);
        }

        ImGui::Separator();

        for (size_t i = 0; i < m_streamingVolumes.size(); ++i)
        {
            auto& vol = m_streamingVolumes[i];
            ImGui::PushID(static_cast<int>(i));

            if (ImGui::CollapsingHeader(vol.name.c_str()))
            {
                ImGui::DragFloat3("Center", &vol.center.x, 1.0f);
                ImGui::DragFloat3("Size", &vol.size.x, 1.0f, 1.0f, 10000.0f);
                ImGui::Checkbox("Active", &vol.isActive);
                ImGui::Text("Player Inside: %s", vol.playerInside ? "Yes" : "No");

                if (ImGui::Button("Remove"))
                {
                    RemoveStreamingVolume(vol.name);
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }
    }

    void LevelStreamingSystem::RenderWorldSettings()
    {
        ImGui::Text("Grid Settings");
        ImGui::DragFloat2("Tile Size", &m_worldSettings.tileSize.x, 10.0f, 100.0f, 10000.0f);
        ImGui::DragInt("Max Tiles X", &m_worldSettings.maxTilesX, 1, 1, 256);
        ImGui::DragInt("Max Tiles Y", &m_worldSettings.maxTilesY, 1, 1, 256);

        ImGui::Separator();
        ImGui::Text("Streaming Settings");
        ImGui::DragFloat("Default Stream Distance", &m_worldSettings.defaultStreamingDistance, 10.0f, 100.0f, 50000.0f);
        ImGui::DragFloat("Default Unload Distance", &m_worldSettings.defaultUnloadingDistance, 10.0f, 100.0f, 50000.0f);
        ImGui::Checkbox("Predictive Streaming", &m_worldSettings.enablePredictiveStreaming);
        ImGui::DragFloat("Prediction Time (s)", &m_worldSettings.predictionTime, 0.1f, 0.0f, 10.0f);

        ImGui::Separator();
        ImGui::Text("Memory Settings");
        int maxMemMB = static_cast<int>(m_worldSettings.maxMemoryBudget / (1024 * 1024));
        if (ImGui::DragInt("Max Memory (MB)", &maxMemMB, 64, 256, 8192))
            m_worldSettings.maxMemoryBudget = static_cast<size_t>(maxMemMB) * 1024 * 1024;
        ImGui::Checkbox("Memory Pressure Unloading", &m_worldSettings.enableMemoryPressureUnloading);

        ImGui::Separator();
        ImGui::Text("LOD Settings");
        ImGui::Checkbox("Enable LOD", &m_worldSettings.enableLOD);
        ImGui::DragFloat("LOD Bias", &m_worldSettings.lodBias, 0.1f, 0.1f, 4.0f);

        ImGui::Separator();
        ImGui::Text("Performance");
        ImGui::DragInt("Max Concurrent Loads", &m_worldSettings.maxConcurrentLoads, 1, 1, 16);
        ImGui::Checkbox("Background Loading", &m_worldSettings.loadInBackground);

        ImGui::Separator();
        ImGui::Text("Debug");
        ImGui::Checkbox("Show Debug Info Tab", &m_showDebugInfo);
        ImGui::Checkbox("Show Tile Bounds", &m_worldSettings.showTileBounds);
        ImGui::Checkbox("Show Streaming Volumes", &m_worldSettings.showStreamingVolumes);
    }

    void LevelStreamingSystem::RenderStreamingStatistics()
    {
        ImGui::Text("Tile Statistics");
        ImGui::Text("Total Tiles: %d", m_statistics.totalTiles);
        ImGui::Text("Loaded: %d  |  Loading: %d  |  Unloading: %d", m_statistics.loadedTiles, m_statistics.loadingTiles,
                    m_statistics.unloadingTiles);

        ImGui::Separator();
        ImGui::Text("Memory");
        ImGui::Text("Current: %.1f MB  |  Peak: %.1f MB", m_statistics.memoryUsage / (1024.0f * 1024.0f),
                    m_statistics.peakMemoryUsage / (1024.0f * 1024.0f));
        float memoryPercent =
            m_worldSettings.maxMemoryBudget > 0
                ? (static_cast<float>(m_statistics.memoryUsage) / static_cast<float>(m_worldSettings.maxMemoryBudget))
                : 0.0f;
        ImGui::ProgressBar(memoryPercent, ImVec2(-1, 0), "Memory Budget");

        ImGui::Separator();
        ImGui::Text("Performance");
        ImGui::Text("Avg Load Time: %.1f ms  |  Avg Unload Time: %.1f ms", m_statistics.averageLoadTime * 1000.0f,
                    m_statistics.averageUnloadTime * 1000.0f);
        ImGui::Text("Load Requests: %d  |  Failed: %d", m_statistics.loadRequests, m_statistics.failedLoads);
        ImGui::Text("Streaming Overhead: %.2f ms/frame", m_statistics.streamingOverhead * 1000.0f);
    }

    void LevelStreamingSystem::RenderDebugInfo()
    {
        ImGui::Text("Viewer Position: (%.1f, %.1f, %.1f)", m_streamingViewer.position.x, m_streamingViewer.position.y,
                    m_streamingViewer.position.z);
        ImGui::Text("Viewer Velocity: (%.1f, %.1f, %.1f)", m_streamingViewer.velocity.x, m_streamingViewer.velocity.y,
                    m_streamingViewer.velocity.z);

        ImGui::Separator();
        ImGui::Text("Loading Queue Size: %d", static_cast<int>(m_loadingQueue.size()));
        ImGui::Text("Unloading Queue Size: %d", static_cast<int>(m_unloadingQueue.size()));
        ImGui::Text("Background Threads: %d", static_cast<int>(m_loadingThreads.size()));
        ImGui::Text("Automatic Streaming: %s", m_automaticStreaming ? "ON" : "OFF");
        ImGui::Text("Streaming Paused: %s", m_streamingPaused ? "YES" : "NO");

        ImGui::Separator();
        ImGui::Text("Per-Tile Debug:");
        for (const auto& tile : m_tiles)
        {
            float dist = tile.GetDistanceToCenter(m_streamingViewer.position);
            ImGui::Text("  %s: dist=%.0f LOD=%d state=%d mem=%zuKB", tile.name.c_str(), dist,
                        static_cast<int>(tile.currentLOD), static_cast<int>(tile.state), tile.memoryUsage / 1024);
        }
    }

    // --- Private streaming update methods ---

    void LevelStreamingSystem::UpdateAutomaticStreaming()
    {
        UpdateDistanceBasedStreaming();
        UpdateTriggerBasedStreaming();
        if (m_worldSettings.enablePredictiveStreaming)
            UpdatePredictiveStreaming();
    }

    void LevelStreamingSystem::UpdateDistanceBasedStreaming()
    {
        for (auto& tile : m_tiles)
        {
            if (tile.streamingMethod != StreamingMethod::DISTANCE_BASED && !tile.alwaysLoaded)
                continue;

            float distance = tile.GetDistanceToBounds(m_streamingViewer.position);

            if (tile.alwaysLoaded && tile.state == StreamingState::UNLOADED)
            {
                RequestTileLoad(tile.name, tile.priority + 100);
                continue;
            }

            if (tile.state == StreamingState::UNLOADED && distance <= tile.streamingDistance)
            {
                int priority = CalculateTilePriority(tile);
                RequestTileLoad(tile.name, priority);
            }
            else if (tile.state == StreamingState::LOADED && distance > tile.unloadingDistance && !tile.alwaysLoaded)
            {
                RequestTileUnload(tile.name);
            }
        }
    }

    void LevelStreamingSystem::UpdateTriggerBasedStreaming()
    {
        for (auto& vol : m_streamingVolumes)
        {
            if (!vol.isActive)
                continue;

            bool inside = vol.ContainsPoint(m_streamingViewer.position);
            if (inside && !vol.playerInside)
            {
                // Entered volume
                vol.playerInside = true;
                for (const auto& tileName : vol.tilesToLoad)
                    RequestTileLoad(tileName, 10);
                for (const auto& tileName : vol.tilesToUnload)
                    RequestTileUnload(tileName);
            }
            else if (!inside && vol.playerInside)
            {
                // Exited volume
                vol.playerInside = false;
            }
        }
    }

    void LevelStreamingSystem::UpdatePredictiveStreaming()
    {
        XMFLOAT3 predicted = m_streamingViewer.GetPredictedPosition(m_worldSettings.predictionTime);
        for (auto& tile : m_tiles)
        {
            if (tile.state != StreamingState::UNLOADED)
                continue;
            float distance = tile.GetDistanceToBounds(predicted);
            if (distance <= tile.streamingDistance)
            {
                int priority = CalculateTilePriority(tile) - 5; // Lower priority than immediate needs
                RequestTileLoad(tile.name, std::max(0, priority));
            }
        }
    }

    void LevelStreamingSystem::UpdateMemoryManagement()
    {
        if (!m_worldSettings.enableMemoryPressureUnloading)
            return;

        if (m_statistics.memoryUsage > m_worldSettings.softMemoryLimit)
        {
            size_t toFree = m_statistics.memoryUsage - m_worldSettings.softMemoryLimit;
            FreeMemory(toFree);
        }
    }

    void LevelStreamingSystem::UpdateLODSystem()
    {
        if (!m_worldSettings.enableLOD)
            return;

        for (auto& tile : m_tiles)
        {
            if (tile.state != StreamingState::LOADED)
                continue;
            float distance = tile.GetDistanceToCenter(m_streamingViewer.position);
            tile.currentLOD = tile.CalculateLOD(distance * m_worldSettings.lodBias);
        }
    }

    void LevelStreamingSystem::ProcessLoadingQueue()
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        int processed = 0;
        while (!m_loadingQueue.empty() && processed < m_worldSettings.maxConcurrentLoads)
        {
            LoadingRequest req = m_loadingQueue.top();
            m_loadingQueue.pop();
            LoadTileSync(req.tileName);
            ++processed;
        }
    }

    void LevelStreamingSystem::ProcessUnloadingQueue()
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        int processed = 0;
        while (!m_unloadingQueue.empty() && processed < m_worldSettings.maxConcurrentLoads)
        {
            std::string tileName = m_unloadingQueue.front();
            m_unloadingQueue.pop();
            UnloadTileSync(tileName);
            ++processed;
        }
    }

    void LevelStreamingSystem::BackgroundLoadingFunction()
    {
        while (!m_shouldStopLoading.load())
        {
            std::string tileName;
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_loadingCondition.wait(lock, [this] { return m_shouldStopLoading.load() || !m_loadingQueue.empty(); });
                if (m_shouldStopLoading.load())
                    break;
                if (m_loadingQueue.empty())
                    continue;
                tileName = m_loadingQueue.top().tileName;
                m_loadingQueue.pop();
            }
            LoadTileSync(tileName);
        }
    }

    bool LevelStreamingSystem::LoadTileSync(const std::string& tileName)
    {
        WorldTile* tile = GetTile(tileName);
        if (!tile)
            return false;
        if (tile->state == StreamingState::LOADED)
            return true;

        tile->state = StreamingState::LOADING;
        tile->loadingProgress = 0.0f;

        // Load the tile's scene file from disk if a path is set
        if (!tile->filePath.empty())
        {
            std::ifstream file(tile->filePath, std::ios::binary | std::ios::ate);
            if (file.is_open())
            {
                auto fileSize = file.tellg();
                tile->memoryUsage = static_cast<size_t>(fileSize);
                file.close();
                tile->loadingProgress = 0.5f;

                // Read the raw data (the scene serializer can parse it later)
                tile->loadedData.resize(tile->memoryUsage);
                std::ifstream dataFile(tile->filePath, std::ios::binary);
                if (dataFile.is_open())
                {
                    dataFile.read(reinterpret_cast<char*>(tile->loadedData.data()),
                                  static_cast<std::streamsize>(tile->memoryUsage));
                }
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loaded tile '%s' from '%s' (%zu bytes)", tileName.c_str(),
                               tile->filePath.c_str(), tile->memoryUsage);
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "Tile file not found: '%s'", tile->filePath.c_str());
                tile->memoryUsage = static_cast<size_t>(tile->worldSize.x * tile->worldSize.z * 4);
            }
        }
        else
        {
            // No file path — estimate memory from tile dimensions
            tile->memoryUsage = static_cast<size_t>(tile->worldSize.x * tile->worldSize.z * 4);
        }

        tile->loadingProgress = 1.0f;
        tile->state = StreamingState::LOADED;
        tile->lastUpdateTime = 0.0f;
        return true;
    }

    bool LevelStreamingSystem::UnloadTileSync(const std::string& tileName)
    {
        WorldTile* tile = GetTile(tileName);
        if (!tile)
            return false;
        if (tile->state != StreamingState::LOADED)
            return false;

        tile->state = StreamingState::UNLOADING;
        tile->memoryUsage = 0;
        tile->loadingProgress = 0.0f;
        tile->state = StreamingState::UNLOADED;
        return true;
    }

    int LevelStreamingSystem::CalculateTilePriority(const WorldTile& tile) const
    {
        float distance = tile.GetDistanceToCenter(m_streamingViewer.position);
        // Closer tiles get higher priority
        int distancePriority = static_cast<int>(std::max(0.0f, 100.0f - distance * 0.01f));
        // Factor in the tile's base priority
        return tile.priority + distancePriority;
    }

    size_t LevelStreamingSystem::GetTileMemoryUsage(const std::string& tileName) const
    {
        for (const auto& tile : m_tiles)
        {
            if (tile.name == tileName)
                return tile.memoryUsage;
        }
        return 0;
    }

    size_t LevelStreamingSystem::FreeMemory(size_t targetMemory)
    {
        // Build list of loaded, non-essential tiles sorted by priority (ascending)
        std::vector<WorldTile*> candidates;
        for (auto& tile : m_tiles)
        {
            if (tile.state == StreamingState::LOADED && !tile.alwaysLoaded)
                candidates.push_back(&tile);
        }

        std::sort(candidates.begin(), candidates.end(), [this](const WorldTile* a, const WorldTile* b)
                  { return CalculateTilePriority(*a) < CalculateTilePriority(*b); });

        size_t freed = 0;
        for (auto* tile : candidates)
        {
            if (freed >= targetMemory)
                break;
            freed += tile->memoryUsage;
            UnloadTileSync(tile->name);
        }
        return freed;
    }

    void LevelStreamingSystem::UpdateStatistics()
    {
        m_statistics.totalTiles = static_cast<int>(m_tiles.size());
        m_statistics.loadedTiles = 0;
        m_statistics.loadingTiles = 0;
        m_statistics.unloadingTiles = 0;
        m_statistics.memoryUsage = 0;

        for (const auto& tile : m_tiles)
        {
            switch (tile.state)
            {
            case StreamingState::LOADED:
                m_statistics.loadedTiles++;
                break;
            case StreamingState::LOADING:
                m_statistics.loadingTiles++;
                break;
            case StreamingState::UNLOADING:
                m_statistics.unloadingTiles++;
                break;
            default:
                break;
            }
            m_statistics.memoryUsage += tile.memoryUsage;
        }

        if (m_statistics.memoryUsage > m_statistics.peakMemoryUsage)
            m_statistics.peakMemoryUsage = m_statistics.memoryUsage;
    }

} // namespace SparkEditor
