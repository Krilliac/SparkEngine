#include "LevelStreamingSystem.h"
#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"
#include "Utils/MathUtils.h"
#include "Utils/Validate.h"
#include <imgui.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <chrono>
#include <iterator>

using namespace DirectX;
namespace SparkEditor
{

    namespace
    {
        /// Semantic status of a tile's streaming state; the tile list shows it as glyph + word and the
        /// overview canvas tints tiles with the same status colour (named once in the canvas legend).
        Spark::Ui::Status TileStatus(StreamingState state)
        {
            switch (state)
            {
            case StreamingState::LOADED:
                return Spark::Ui::Status::Success;
            case StreamingState::LOADING:
                return Spark::Ui::Status::Info;
            case StreamingState::UNLOADING:
                return Spark::Ui::Status::Warning;
            case StreamingState::FAILED:
                return Spark::Ui::Status::Error;
            case StreamingState::UNLOADED:
            default:
                return Spark::Ui::Status::Neutral;
            }
        }

        const char* TileStateName(StreamingState state)
        {
            switch (state)
            {
            case StreamingState::LOADED:
                return "Loaded";
            case StreamingState::LOADING:
                return "Loading";
            case StreamingState::UNLOADING:
                return "Unloading";
            case StreamingState::FAILED:
                return "Failed";
            case StreamingState::UNLOADED:
            default:
                return "Unloaded";
            }
        }

        /// Opacity of a tile's state tint on the overview canvas.
        constexpr float kTileFillAlpha = 0.55f;
    } // namespace

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
        {
            return outsideDist;
        }

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
            {
                return static_cast<LODLevel>(i);
            }
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
        {
            return true;
        }

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
        // Publish the stop flag under the waiter's mutex: a worker that has already
        // evaluated its wait predicate but not yet blocked would otherwise miss this
        // notification and the join below would hang.
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_shouldStopLoading.store(true);
        }
        m_loadingCondition.notify_all();
        for (auto& thread : m_loadingThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
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
        {
            return;
        }

        if (m_automaticStreaming)
        {
            UpdateAutomaticStreaming();
        }

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
        if (Spark::Ui::BeginPanelToolbar("LevelStreamingToolbar"))
        {
            if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " New World"))
            {
                CreateNewWorld("New World", m_worldSettings);
            }
            ImGui::SameLine();
            bool streamingPaused = m_streamingPaused;
            if (ImGui::Checkbox("Pause Streaming", &streamingPaused))
            {
                m_streamingPaused = streamingPaused;
            }

            Spark::Ui::ToolbarSeparator();
            ImGui::AlignTextToFramePadding();
            Spark::Ui::KeyValue("World:", "%s", m_worldName.c_str());
            ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
            Spark::Ui::KeyValue("Tiles:", "%d", static_cast<int>(m_tiles.size()));
            Spark::Ui::EndPanelToolbar();
        }

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
        // Publish the stop flag under the waiter's mutex: a worker that has already
        // evaluated its wait predicate but not yet blocked would otherwise miss this
        // notification and the join below would hang.
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_shouldStopLoading.store(true);
        }
        m_loadingCondition.notify_all();
        for (auto& thread : m_loadingThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
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
        {
            return false;
        }
        m_worldName.resize(nameLen);
        file.read(m_worldName.data(), nameLen);

        // Read tile count
        uint32_t tileCount = 0;
        file.read(reinterpret_cast<char*>(&tileCount), sizeof(tileCount));
        if (tileCount > 100000)
        {
            return false;
        }

        m_tiles.clear();
        m_tiles.reserve(tileCount);
        for (uint32_t i = 0; i < tileCount; ++i)
        {
            WorldTile tile;
            // Read tile name
            uint32_t tileNameLen = 0;
            file.read(reinterpret_cast<char*>(&tileNameLen), sizeof(tileNameLen));
            if (tileNameLen > 4096)
            {
                return false;
            }
            tile.name.resize(tileNameLen);
            file.read(tile.name.data(), tileNameLen);

            // Read tile file path
            uint32_t pathLen = 0;
            file.read(reinterpret_cast<char*>(&pathLen), sizeof(pathLen));
            if (pathLen > 4096)
            {
                return false;
            }
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
        {
            return false;
        }

        m_streamingVolumes.clear();
        m_streamingVolumes.reserve(volumeCount);
        for (uint32_t i = 0; i < volumeCount; ++i)
        {
            StreamingVolume vol;
            uint32_t volNameLen = 0;
            file.read(reinterpret_cast<char*>(&volNameLen), sizeof(volNameLen));
            if (volNameLen > 4096)
            {
                return false;
            }
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
        {
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Removing tile: '%s'", tileName.c_str());

        if (it->state == StreamingState::LOADED)
        {
            UnloadTileSync(tileName);
        }

        if (m_selectedTile == tileName)
        {
            m_selectedTile.clear();
        }

        m_tiles.erase(it);
        SetModified(true);
        return true;
    }

    WorldTile* LevelStreamingSystem::GetTile(const std::string& tileName)
    {
        for (auto& tile : m_tiles)
        {
            if (tile.name == tileName)
            {
                return &tile;
            }
        }
        return nullptr;
    }

    WorldTile* LevelStreamingSystem::GetTileAtPosition(const XMFLOAT3& worldPosition)
    {
        for (auto& tile : m_tiles)
        {
            if (tile.ContainsPoint(worldPosition))
            {
                return &tile;
            }
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
        {
            return false;
        }
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
        {
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Requesting tile load: '%s' (priority=%d, blocking=%s)",
                       tileName.c_str(), priority, blockOnLoad ? "true" : "false");

        if (blockOnLoad)
        {
            return LoadTileSync(tileName);
        }

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
        {
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Requesting tile unload: '%s' (immediate=%s)", tileName.c_str(),
                       immediate ? "true" : "false");

        if (immediate)
        {
            return ForceUnloadTile(tileName);
        }

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
            {
                result.push_back(&tile);
            }
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
            {
                bsCenter = tile.worldPosition;
            }
            if (bsRadius <= 0.0f)
            {
                bsRadius = std::max(tile.worldSize.x, tile.worldSize.z) * 0.5f;
            }

            if (viewer.IsInViewFrustum(bsCenter, bsRadius))
            {
                result.push_back(&tile);
            }
        }
        return result;
    }

    void LevelStreamingSystem::UpdateTileLOD(const std::string& tileName, float viewerDistance)
    {
        WorldTile* tile = GetTile(tileName);
        if (!tile)
        {
            return;
        }
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
                {
                    ++count;
                }
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
            {
                loadedTiles.push_back(&tile);
            }
        }

        std::sort(loadedTiles.begin(), loadedTiles.end(),
                  [](const WorldTile* a, const WorldTile* b) { return a->priority < b->priority; });

        int optimized = 0;
        size_t currentMemory = m_statistics.memoryUsage;
        for (auto* tile : loadedTiles)
        {
            if (currentMemory <= targetMemoryUsage)
            {
                break;
            }
            if (tile->alwaysLoaded)
            {
                continue;
            }
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
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Legend: the canvas codes tile state by colour, so each colour is named once here.
        const StreamingState legendStates[] = {StreamingState::LOADED, StreamingState::LOADING,
                                               StreamingState::UNLOADING, StreamingState::FAILED,
                                               StreamingState::UNLOADED};
        const float swatch = ImGui::GetTextLineHeight() * 0.75f;
        for (size_t i = 0; i < std::size(legendStates); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
            }
            const ImVec2 swatchPos = ImGui::GetCursorScreenPos();
            const float inset = (ImGui::GetTextLineHeight() - swatch) * 0.5f;
            drawList->AddRectFilled(
                ImVec2(swatchPos.x, swatchPos.y + inset), ImVec2(swatchPos.x + swatch, swatchPos.y + inset + swatch),
                Spark::Ui::Col(Spark::Ui::StatusTextRole(TileStatus(legendStates[i])), kTileFillAlpha));
            ImGui::Dummy(ImVec2(swatch, ImGui::GetTextLineHeight()));
            ImGui::SameLine(0.0f, Spark::Ui::Em(0.3f));
            Spark::Ui::SecondaryText("%s", TileStateName(legendStates[i]));
        }

        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x < 50.0f || canvasSize.y < 50.0f)
        {
            return;
        }

        ImVec2 canvasPos = ImGui::GetCursorScreenPos();

        // Background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                Spark::Ui::Col(Spark::Ui::Role::SurfaceSunken));

        // Draw grid
        const ImU32 gridColor = Spark::Ui::Col(Spark::Ui::Role::BorderSubtle);
        float gridStep = 50.0f * m_overviewZoom;
        for (float x = 0; x < canvasSize.x; x += gridStep)
        {
            drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y), ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y),
                              gridColor);
        }
        for (float y = 0; y < canvasSize.y; y += gridStep)
        {
            drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y), ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y),
                              gridColor);
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

            const ImU32 color = Spark::Ui::Col(Spark::Ui::StatusTextRole(TileStatus(tile.state)), kTileFillAlpha);

            ImVec2 tileMin = {tileScreenX - halfW, tileScreenY - halfH};
            ImVec2 tileMax = {tileScreenX + halfW, tileScreenY + halfH};
            drawList->AddRectFilled(tileMin, tileMax, color);

            // The selected tile gets the focus hue and a heavier outline, not just a different colour.
            const bool isSelected = (tile.name == m_selectedTile);
            drawList->AddRect(tileMin, tileMax,
                              Spark::Ui::Col(isSelected ? Spark::Ui::Role::BorderFocus : Spark::Ui::Role::BorderStrong),
                              0.0f, 0, isSelected ? Spark::Ui::Px(2.0f) : Spark::Ui::Px(1.0f));

            // Tile label
            if (halfW > 10.0f)
            {
                const float labelInset = Spark::Ui::Px(2.0f);
                drawList->AddText(ImVec2(tileMin.x + labelInset, tileMin.y + labelInset),
                                  Spark::Ui::Col(Spark::Ui::Role::TextPrimary, .78f), tile.name.c_str());
            }
        }

        // Draw viewer position
        float viewerX = centerX + m_streamingViewer.position.x * scale;
        float viewerY = centerY + m_streamingViewer.position.z * scale;
        drawList->AddCircleFilled(ImVec2(viewerX, viewerY), Spark::Ui::Px(5.0f),
                                  Spark::Ui::Col(Spark::Ui::Role::AccentFill));

        // Handle mouse interaction for pan/zoom
        ImGui::InvisibleButton("world_overview_canvas", canvasSize);
        if (ImGui::IsItemHovered())
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                m_overviewZoom = std::clamp(m_overviewZoom + wheel * 0.1f, 0.1f, 10.0f);
            }

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
        if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " Add Tile"))
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
            if (m_tiles.empty())
            {
                Spark::Ui::EmptyState(ICON_FA_TH, "No tiles", "Add a tile or create a new world to start streaming.");
            }

            for (size_t i = 0; i < m_tiles.size(); ++i)
            {
                auto& tile = m_tiles[i];
                bool isSelected = (tile.name == m_selectedTile);

                ImGui::PushID(static_cast<int>(i));
                Spark::Ui::StatusText(TileStatus(tile.state), "%s", TileStateName(tile.state));
                ImGui::SameLine(0.0f, Spark::Ui::Em(0.6f));

                if (ImGui::Selectable(tile.name.c_str(), isSelected))
                {
                    m_selectedTile = tile.name;
                }

                if (isSelected && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    // Double-click to load/unload
                    if (tile.state == StreamingState::UNLOADED)
                    {
                        RequestTileLoad(tile.name, tile.priority);
                    }
                    else if (tile.state == StreamingState::LOADED)
                    {
                        RequestTileUnload(tile.name);
                    }
                }

                // Tile details on selection
                if (isSelected)
                {
                    ImGui::Indent();
                    Spark::Ui::KeyValue("Position:", "(%.0f, %.0f, %.0f)", tile.worldPosition.x, tile.worldPosition.y,
                                        tile.worldPosition.z);
                    Spark::Ui::KeyValue("Size:", "(%.0f, %.0f, %.0f)", tile.worldSize.x, tile.worldSize.y,
                                        tile.worldSize.z);
                    Spark::Ui::KeyValue("LOD:", "%d", static_cast<int>(tile.currentLOD));
                    ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
                    Spark::Ui::KeyValue("Priority:", "%d", tile.priority);
                    ImGui::SliderFloat("Stream Dist", &tile.streamingDistance, 100.0f, 10000.0f);
                    ImGui::SliderFloat("Unload Dist", &tile.unloadingDistance, 100.0f, 15000.0f);
                    ImGui::Checkbox("Always Loaded", &tile.alwaysLoaded);

                    if (tile.state == StreamingState::UNLOADED && Spark::Ui::SecondaryButton("Load"))
                    {
                        RequestTileLoad(tile.name, tile.priority);
                    }
                    if (tile.state == StreamingState::LOADED)
                    {
                        ImGui::SameLine();
                        if (Spark::Ui::SecondaryButton("Unload"))
                        {
                            RequestTileUnload(tile.name);
                        }
                    }

                    ImGui::SameLine();
                    if (Spark::Ui::DangerButton(ICON_FA_TRASH " Remove"))
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
        if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " Add Volume"))
        {
            StreamingVolume vol;
            vol.name = "Volume_" + std::to_string(m_streamingVolumes.size());
            AddStreamingVolume(vol);
        }

        ImGui::Separator();

        if (m_streamingVolumes.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_VECTOR_SQUARE, "No streaming volumes",
                                  "Add a volume to stream tiles when the viewer enters it.");
            return;
        }

        for (size_t i = 0; i < m_streamingVolumes.size(); ++i)
        {
            auto& vol = m_streamingVolumes[i];
            ImGui::PushID(static_cast<int>(i));

            if (Spark::Ui::SectionHeader(vol.name.c_str(), nullptr, false))
            {
                ImGui::DragFloat3("Center", &vol.center.x, 1.0f);
                ImGui::DragFloat3("Size", &vol.size.x, 1.0f, 1.0f, 10000.0f);
                ImGui::Checkbox("Active", &vol.isActive);
                Spark::Ui::KeyValue("Player Inside:", "%s", vol.playerInside ? "Yes" : "No");

                if (Spark::Ui::DangerButton(ICON_FA_TRASH " Remove"))
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
        Spark::Ui::SubHeader("Grid Settings");
        ImGui::DragFloat2("Tile Size", &m_worldSettings.tileSize.x, 10.0f, 100.0f, 10000.0f);
        ImGui::DragInt("Max Tiles X", &m_worldSettings.maxTilesX, 1, 1, 256);
        ImGui::DragInt("Max Tiles Y", &m_worldSettings.maxTilesY, 1, 1, 256);

        Spark::Ui::SubHeader("Streaming Settings");
        ImGui::DragFloat("Default Stream Distance", &m_worldSettings.defaultStreamingDistance, 10.0f, 100.0f, 50000.0f);
        ImGui::DragFloat("Default Unload Distance", &m_worldSettings.defaultUnloadingDistance, 10.0f, 100.0f, 50000.0f);
        ImGui::Checkbox("Predictive Streaming", &m_worldSettings.enablePredictiveStreaming);
        ImGui::DragFloat("Prediction Time (s)", &m_worldSettings.predictionTime, 0.1f, 0.0f, 10.0f);

        Spark::Ui::SubHeader("Memory Settings");
        int maxMemMB = static_cast<int>(m_worldSettings.maxMemoryBudget / (1024 * 1024));
        if (ImGui::DragInt("Max Memory (MB)", &maxMemMB, 64, 256, 8192))
        {
            m_worldSettings.maxMemoryBudget = static_cast<size_t>(maxMemMB) * 1024 * 1024;
        }
        ImGui::Checkbox("Memory Pressure Unloading", &m_worldSettings.enableMemoryPressureUnloading);

        Spark::Ui::SubHeader("LOD Settings");
        ImGui::Checkbox("Enable LOD", &m_worldSettings.enableLOD);
        ImGui::DragFloat("LOD Bias", &m_worldSettings.lodBias, 0.1f, 0.1f, 4.0f);

        Spark::Ui::SubHeader("Performance");
        ImGui::DragInt("Max Concurrent Loads", &m_worldSettings.maxConcurrentLoads, 1, 1, 16);
        ImGui::Checkbox("Background Loading", &m_worldSettings.loadInBackground);

        Spark::Ui::SubHeader("Debug");
        ImGui::Checkbox("Show Debug Info Tab", &m_showDebugInfo);
        ImGui::Checkbox("Show Tile Bounds", &m_worldSettings.showTileBounds);
        ImGui::Checkbox("Show Streaming Volumes", &m_worldSettings.showStreamingVolumes);
    }

    void LevelStreamingSystem::RenderStreamingStatistics()
    {
        Spark::Ui::SubHeader("Tile Statistics");
        Spark::Ui::KeyValue("Total Tiles:", "%d", m_statistics.totalTiles);
        Spark::Ui::KeyValue("Loaded:", "%d", m_statistics.loadedTiles);
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Loading:", "%d", m_statistics.loadingTiles);
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Unloading:", "%d", m_statistics.unloadingTiles);

        Spark::Ui::SubHeader("Memory");
        Spark::Ui::KeyValue("Current:", "%.1f MB", m_statistics.memoryUsage / (1024.0f * 1024.0f));
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Peak:", "%.1f MB", m_statistics.peakMemoryUsage / (1024.0f * 1024.0f));
        float memoryPercent =
            m_worldSettings.maxMemoryBudget > 0
                ? (static_cast<float>(m_statistics.memoryUsage) / static_cast<float>(m_worldSettings.maxMemoryBudget))
                : 0.0f;
        ImGui::ProgressBar(memoryPercent, ImVec2(-1, 0), "Memory Budget");

        Spark::Ui::SubHeader("Performance");
        Spark::Ui::KeyValue("Avg Load Time:", "%.1f ms", m_statistics.averageLoadTime * 1000.0f);
        Spark::Ui::KeyValue("Avg Unload Time:", "%.1f ms", m_statistics.averageUnloadTime * 1000.0f);
        Spark::Ui::KeyValue("Load Requests:", "%d", m_statistics.loadRequests);
        ImGui::SameLine(0.0f, Spark::Ui::Em(1.0f));
        Spark::Ui::KeyValue("Failed:", "%d", m_statistics.failedLoads);
        Spark::Ui::KeyValue("Streaming Overhead:", "%.2f ms/frame", m_statistics.streamingOverhead * 1000.0f);
    }

    void LevelStreamingSystem::RenderDebugInfo()
    {
        Spark::Ui::KeyValue("Viewer Position:", "(%.1f, %.1f, %.1f)", m_streamingViewer.position.x,
                            m_streamingViewer.position.y, m_streamingViewer.position.z);
        Spark::Ui::KeyValue("Viewer Velocity:", "(%.1f, %.1f, %.1f)", m_streamingViewer.velocity.x,
                            m_streamingViewer.velocity.y, m_streamingViewer.velocity.z);

        Spark::Ui::SubHeader("Queues");
        Spark::Ui::KeyValue("Loading Queue Size:", "%d", static_cast<int>(m_loadingQueue.size()));
        Spark::Ui::KeyValue("Unloading Queue Size:", "%d", static_cast<int>(m_unloadingQueue.size()));
        Spark::Ui::KeyValue("Background Threads:", "%d", static_cast<int>(m_loadingThreads.size()));
        Spark::Ui::KeyValue("Automatic Streaming:", "%s", m_automaticStreaming ? "ON" : "OFF");
        Spark::Ui::KeyValue("Streaming Paused:", "%s", m_streamingPaused ? "YES" : "NO");

        Spark::Ui::SubHeader("Per-Tile Debug");
        for (const auto& tile : m_tiles)
        {
            float dist = tile.GetDistanceToCenter(m_streamingViewer.position);
            Spark::Ui::MonoText("%s: dist=%.0f LOD=%d state=%s mem=%zuKB", tile.name.c_str(), dist,
                                static_cast<int>(tile.currentLOD), TileStateName(tile.state), tile.memoryUsage / 1024);
        }
    }

    // --- Private streaming update methods ---

    void LevelStreamingSystem::UpdateAutomaticStreaming()
    {
        UpdateDistanceBasedStreaming();
        UpdateTriggerBasedStreaming();
        if (m_worldSettings.enablePredictiveStreaming)
        {
            UpdatePredictiveStreaming();
        }
    }

    void LevelStreamingSystem::UpdateDistanceBasedStreaming()
    {
        for (auto& tile : m_tiles)
        {
            if (tile.streamingMethod != StreamingMethod::DISTANCE_BASED && !tile.alwaysLoaded)
            {
                continue;
            }

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
            {
                continue;
            }

            bool inside = vol.ContainsPoint(m_streamingViewer.position);
            if (inside && !vol.playerInside)
            {
                // Entered volume
                vol.playerInside = true;
                for (const auto& tileName : vol.tilesToLoad)
                {
                    RequestTileLoad(tileName, 10);
                }
                for (const auto& tileName : vol.tilesToUnload)
                {
                    RequestTileUnload(tileName);
                }
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
            {
                continue;
            }
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
        {
            return;
        }

        if (m_statistics.memoryUsage > m_worldSettings.softMemoryLimit)
        {
            size_t toFree = m_statistics.memoryUsage - m_worldSettings.softMemoryLimit;
            FreeMemory(toFree);
        }
    }

    void LevelStreamingSystem::UpdateLODSystem()
    {
        if (!m_worldSettings.enableLOD)
        {
            return;
        }

        for (auto& tile : m_tiles)
        {
            if (tile.state != StreamingState::LOADED)
            {
                continue;
            }
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
                {
                    break;
                }
                if (m_loadingQueue.empty())
                {
                    continue;
                }
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
        {
            return false;
        }
        if (tile->state == StreamingState::LOADED)
        {
            return true;
        }

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
        {
            return false;
        }
        if (tile->state != StreamingState::LOADED)
        {
            return false;
        }

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
            {
                return tile.memoryUsage;
            }
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
            {
                candidates.push_back(&tile);
            }
        }

        std::sort(candidates.begin(), candidates.end(), [this](const WorldTile* a, const WorldTile* b)
                  { return CalculateTilePriority(*a) < CalculateTilePriority(*b); });

        size_t freed = 0;
        for (auto* tile : candidates)
        {
            if (freed >= targetMemory)
            {
                break;
            }
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
        {
            m_statistics.peakMemoryUsage = m_statistics.memoryUsage;
        }
    }

} // namespace SparkEditor
