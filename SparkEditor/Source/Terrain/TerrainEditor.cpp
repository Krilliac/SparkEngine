/**
 * @file TerrainEditor.cpp
 * @brief Core terrain editor: lifecycle, load/save, undo/redo, mesh/collision rebuild
 */

#include "TerrainEditor.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include "../Core/EditorIcons.h"
#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/TerrainComponents.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Graphics/TerrainRenderer.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace DirectX;
namespace SparkEditor
{

    namespace
    {
        namespace Format = Spark::Graphics::SparkTerrain;

        void WriteU8(std::ofstream& file, uint8_t value)
        {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }

        void WriteU32(std::ofstream& file, uint32_t value)
        {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }

        void WriteI32(std::ofstream& file, int32_t value)
        {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }

        void WriteF32(std::ofstream& file, float value)
        {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        }

        void WriteString(std::ofstream& file, const std::string& value)
        {
            const auto length = static_cast<uint32_t>(std::min<size_t>(value.size(), Format::kMaxStringLength));
            WriteU32(file, length);
            if (length > 0)
                file.write(value.data(), static_cast<std::streamsize>(length));
        }
    } // namespace

    TerrainEditor::TerrainEditor() : EditorPanel("Terrain Editor", "terrain_editor") {}

    TerrainEditor::~TerrainEditor() = default;

    bool TerrainEditor::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Terrain Editor panel");
        return true;
    }

    void TerrainEditor::Update(float deltaTime)
    {
        if (m_isApplyingTool)
        {
            m_lastToolTime += deltaTime;
        }

        // Deferred mesh/collision rebuild after tool strokes
        if (m_meshDirty)
            UpdateTerrainMesh();
        if (m_collisionDirty)
            UpdateTerrainCollision();
    }

    void TerrainEditor::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (!m_currentTerrain)
            {
                RenderNoTerrainView();
            }
            else
            {
                RenderToolPalette();
                ImGui::Separator();
                RenderBrushSettings();
                ImGui::Separator();

                if (m_showHeightmapTools)
                    RenderHeightmapTools();
                if (m_showTexturePainting)
                    RenderTexturePaintingTools();
                if (m_showDetailPlacement)
                    RenderDetailPlacementTools();
                if (m_showGenerationTools)
                    RenderGenerationTools();

                ImGui::Separator();
                RenderTerrainProperties();
            }
        }
        EndPanel();
    }

    void TerrainEditor::Shutdown()
    {
        std::cout << "Shutting down Terrain Editor panel\n";
        m_currentTerrain.reset();
        m_undoStack.clear();
        m_redoStack.clear();
        m_meshVertices.clear();
        m_meshNormals.clear();
        m_meshIndices.clear();
        m_collisionHeights.clear();
    }

    bool TerrainEditor::HandleEvent(const std::string& eventType, void* /*eventData*/)
    {
        if (eventType == "terrain_create")
        {
            CreateNewTerrain();
            return true;
        }
        return false;
    }

    void TerrainEditor::CreateNewTerrain(float size, int heightmapResolution, const XMFLOAT3& position)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Creating new terrain: size=%.1f, resolution=%d", size,
                       heightmapResolution);
        m_currentTerrain = std::make_unique<TerrainData>();
        m_currentTerrain->name = "New Terrain";
        m_currentTerrain->size = size;
        m_currentTerrain->position = position;
        m_currentTerrain->heightmap.width = heightmapResolution;
        m_currentTerrain->heightmap.height = heightmapResolution;
        m_currentTerrain->heightmap.heights.resize(
            static_cast<size_t>(heightmapResolution) * static_cast<size_t>(heightmapResolution), 0.0f);

        m_currentTerrain->splatmaps.resize(static_cast<size_t>(m_currentTerrain->splatmapResolution) *
                                               static_cast<size_t>(m_currentTerrain->splatmapResolution) * 4,
                                           0);
        m_currentTerrain->AddTextureLayer("Grass");

        m_undoStack.clear();
        m_redoStack.clear();
        m_terrainFilePath.clear();
        RefreshPathBuffers();
        UpdateTerrainMesh();
        UpdateTerrainCollision();
        SetModified(true);
    }

    bool TerrainEditor::LoadTerrain(const std::string& filePath)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading terrain from: %s", filePath.c_str());
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to open terrain file: %s", filePath.c_str());
            return false;
        }

        Format::Reader reader(file);

        uint32_t magic = 0;
        uint32_t version = 0;
        reader.ReadU32(magic);
        reader.ReadU32(version);
        if (reader.Failed() || magic != Format::kMagic)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Not a .sparkterrain file: %s", filePath.c_str());
            return false;
        }
        if (version != Format::kVersion)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Unsupported .sparkterrain version %u (expected %u): %s",
                            version, Format::kVersion, filePath.c_str());
            return false;
        }

        auto terrain = std::make_unique<TerrainData>();

        reader.ReadString(terrain->name);
        reader.ReadF32(terrain->size);
        reader.ReadF32(terrain->position.x);
        reader.ReadF32(terrain->position.y);
        reader.ReadF32(terrain->position.z);

        int32_t lodLevels = 4;
        uint8_t generateCollider = 1;
        reader.ReadI32(lodLevels);
        reader.ReadF32(terrain->lodBias);
        reader.ReadU8(generateCollider);
        terrain->lodLevels = lodLevels;
        terrain->generateCollider = (generateCollider != 0);

        int32_t hmWidth = 0;
        int32_t hmHeight = 0;
        reader.ReadI32(hmWidth);
        reader.ReadI32(hmHeight);
        reader.ReadF32(terrain->heightmap.scale);
        reader.ReadF32(terrain->heightmap.minHeight);
        reader.ReadF32(terrain->heightmap.maxHeight);
        if (reader.Failed() || hmWidth < Format::kMinHeightmapResolution || hmWidth > Format::kMaxHeightmapResolution ||
            hmHeight < Format::kMinHeightmapResolution || hmHeight > Format::kMaxHeightmapResolution)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Rejected terrain heightmap %dx%d in %s", hmWidth, hmHeight,
                            filePath.c_str());
            return false;
        }
        terrain->heightmap.width = hmWidth;
        terrain->heightmap.height = hmHeight;

        const uint64_t heightCount = static_cast<uint64_t>(hmWidth) * static_cast<uint64_t>(hmHeight);
        if (heightCount * sizeof(float) > reader.Remaining())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Truncated terrain heightmap in %s", filePath.c_str());
            return false;
        }
        terrain->heightmap.heights.resize(static_cast<size_t>(heightCount));
        reader.ReadBytes(terrain->heightmap.heights.data(), heightCount * sizeof(float));

        uint32_t layerCount = 0;
        reader.ReadU32(layerCount);
        if (reader.Failed() || layerCount > Format::kMaxTextureLayers)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Rejected terrain layer count %u in %s", layerCount,
                            filePath.c_str());
            return false;
        }
        for (uint32_t i = 0; i < layerCount && !reader.Failed(); ++i)
        {
            std::string layerName;
            reader.ReadString(layerName);
            TerrainTextureLayer* layer = terrain->AddTextureLayer(layerName);
            if (!layer)
                return false;
            reader.ReadString(layer->diffuseTexture);
            reader.ReadString(layer->normalTexture);
            reader.ReadString(layer->maskTexture);
            reader.ReadF32(layer->tiling.x);
            reader.ReadF32(layer->tiling.y);
            reader.ReadF32(layer->offset.x);
            reader.ReadF32(layer->offset.y);
            reader.ReadF32(layer->opacity);
            reader.ReadF32(layer->metallic);
            reader.ReadF32(layer->roughness);
            reader.ReadF32(layer->normalStrength);
        }

        int32_t splatRes = 0;
        reader.ReadI32(splatRes);
        if (reader.Failed() || splatRes < 0 || splatRes > Format::kMaxSplatmapResolution)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Rejected terrain splatmap resolution %d in %s", splatRes,
                            filePath.c_str());
            return false;
        }
        terrain->splatmapResolution = splatRes;
        const uint64_t splatSize = static_cast<uint64_t>(splatRes) * static_cast<uint64_t>(splatRes) * 4u;
        if (splatSize > reader.Remaining())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Truncated terrain splatmap in %s", filePath.c_str());
            return false;
        }
        terrain->splatmaps.resize(static_cast<size_t>(splatSize));
        reader.ReadBytes(terrain->splatmaps.data(), splatSize);

        uint32_t detailMeshCount = 0;
        reader.ReadU32(detailMeshCount);
        if (reader.Failed() || detailMeshCount > Format::kMaxDetailMeshes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Rejected terrain detail mesh count %u in %s", detailMeshCount,
                            filePath.c_str());
            return false;
        }
        terrain->detailInstances.resize(detailMeshCount);
        for (uint32_t i = 0; i < detailMeshCount && !reader.Failed(); ++i)
        {
            auto mesh = std::make_unique<TerrainDetailMesh>();
            reader.ReadString(mesh->name);
            reader.ReadString(mesh->meshPath);
            reader.ReadString(mesh->materialPath);
            reader.ReadF32(mesh->density);
            reader.ReadF32(mesh->viewDistance);

            uint32_t instanceCount = 0;
            reader.ReadU32(instanceCount);
            if (reader.Failed() || instanceCount > Format::kMaxDetailInstancesPerMesh)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Rejected terrain detail instance count %u in %s",
                                instanceCount, filePath.c_str());
                return false;
            }
            const uint64_t instanceBytes = static_cast<uint64_t>(instanceCount) * sizeof(XMFLOAT3);
            if (instanceBytes > reader.Remaining())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Truncated terrain detail instances in %s",
                                filePath.c_str());
                return false;
            }
            auto& instances = terrain->detailInstances[i];
            instances.resize(instanceCount);
            if (instanceCount > 0)
                reader.ReadBytes(instances.data(), instanceBytes);

            terrain->detailMeshes.push_back(std::move(mesh));
        }

        if (reader.Failed())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to parse terrain file: %s", filePath.c_str());
            return false;
        }

        m_currentTerrain = std::move(terrain);
        m_terrainFilePath = filePath;
        m_undoStack.clear();
        m_redoStack.clear();
        RefreshPathBuffers();
        UpdateTerrainMesh();
        UpdateTerrainCollision();
        SetModified(false);
        return true;
    }

    std::string TerrainEditor::DefaultTerrainPath(const std::string& terrainName)
    {
        std::string safeName;
        safeName.reserve(terrainName.size());
        for (char c : terrainName)
            safeName.push_back((c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                                c == '>' || c == '|')
                                   ? '_'
                                   : c);
        if (safeName.empty())
            safeName = "terrain";
        return "Assets/Terrains/" + safeName + ".sparkterrain";
    }

    bool TerrainEditor::SaveTerrain(const std::string& filePath)
    {
        if (!m_currentTerrain)
            return false;

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving terrain '%s' to: %s", m_currentTerrain->name.c_str(),
                       filePath.c_str());

        // The readers enforce the SparkTerrain limits and read exactly width*height heights and
        // splatRes*splatRes*4 splat bytes. Writing anything else produces a file that saves cleanly
        // and can never be loaded, so the same limits are enforced here, before a byte is written.
        const TerrainHeightmap& heightmap = m_currentTerrain->heightmap;
        if (heightmap.width < Format::kMinHeightmapResolution || heightmap.width > Format::kMaxHeightmapResolution ||
            heightmap.height < Format::kMinHeightmapResolution || heightmap.height > Format::kMaxHeightmapResolution)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Refusing to save terrain: heightmap %dx%d is outside the supported range [%d, %d]",
                            heightmap.width, heightmap.height, Format::kMinHeightmapResolution,
                            Format::kMaxHeightmapResolution);
            return false;
        }

        const uint64_t expectedHeights =
            static_cast<uint64_t>(heightmap.width) * static_cast<uint64_t>(heightmap.height);
        if (static_cast<uint64_t>(heightmap.heights.size()) != expectedHeights)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Refusing to save terrain: heightmap holds %zu samples but declares %dx%d",
                            heightmap.heights.size(), heightmap.width, heightmap.height);
            return false;
        }

        if (m_currentTerrain->textureLayers.size() > Format::kMaxTextureLayers)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Refusing to save terrain: %zu texture layers exceeds the format limit of %u",
                            m_currentTerrain->textureLayers.size(), Format::kMaxTextureLayers);
            return false;
        }

        if (m_currentTerrain->splatmapResolution < 0 ||
            m_currentTerrain->splatmapResolution > Format::kMaxSplatmapResolution)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Refusing to save terrain: splatmap resolution %d is outside [0, %d]",
                            m_currentTerrain->splatmapResolution, Format::kMaxSplatmapResolution);
            return false;
        }

        const uint64_t expectedSplatBytes = static_cast<uint64_t>(m_currentTerrain->splatmapResolution) *
                                            static_cast<uint64_t>(m_currentTerrain->splatmapResolution) * 4u;
        if (static_cast<uint64_t>(m_currentTerrain->splatmaps.size()) != expectedSplatBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Refusing to save terrain: splatmap holds %zu bytes but resolution %d requires %llu",
                            m_currentTerrain->splatmaps.size(), m_currentTerrain->splatmapResolution,
                            static_cast<unsigned long long>(expectedSplatBytes));
            return false;
        }

        if (m_currentTerrain->detailMeshes.size() > Format::kMaxDetailMeshes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Refusing to save terrain: %zu detail meshes exceeds the format limit of %u",
                            m_currentTerrain->detailMeshes.size(), Format::kMaxDetailMeshes);
            return false;
        }

        for (const auto& instances : m_currentTerrain->detailInstances)
        {
            if (instances.size() > Format::kMaxDetailInstancesPerMesh)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                                "Refusing to save terrain: %zu detail instances exceeds the format limit of %u",
                                instances.size(), Format::kMaxDetailInstancesPerMesh);
                return false;
            }
        }

        const std::filesystem::path target(filePath);
        if (target.has_parent_path() && !target.parent_path().empty())
        {
            std::error_code createError;
            std::filesystem::create_directories(target.parent_path(), createError);
            if (createError)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create terrain directory '%s': %s",
                                target.parent_path().string().c_str(), createError.message().c_str());
                return false;
            }
        }

        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to save terrain to: %s", filePath.c_str());
            return false;
        }

        WriteU32(file, Format::kMagic);
        WriteU32(file, Format::kVersion);

        WriteString(file, m_currentTerrain->name);
        WriteF32(file, m_currentTerrain->size);
        WriteF32(file, m_currentTerrain->position.x);
        WriteF32(file, m_currentTerrain->position.y);
        WriteF32(file, m_currentTerrain->position.z);
        WriteI32(file, m_currentTerrain->lodLevels);
        WriteF32(file, m_currentTerrain->lodBias);
        WriteU8(file, m_currentTerrain->generateCollider ? 1u : 0u);

        WriteI32(file, m_currentTerrain->heightmap.width);
        WriteI32(file, m_currentTerrain->heightmap.height);
        WriteF32(file, m_currentTerrain->heightmap.scale);
        WriteF32(file, m_currentTerrain->heightmap.minHeight);
        WriteF32(file, m_currentTerrain->heightmap.maxHeight);

        // Exactly width*height floats: validated above, so the container and the header agree.
        if (expectedHeights > 0)
        {
            file.write(reinterpret_cast<const char*>(heightmap.heights.data()),
                       static_cast<std::streamsize>(expectedHeights * sizeof(float)));
        }

        WriteU32(file, static_cast<uint32_t>(m_currentTerrain->textureLayers.size()));
        for (const auto& layer : m_currentTerrain->textureLayers)
        {
            WriteString(file, layer->name);
            WriteString(file, layer->diffuseTexture);
            WriteString(file, layer->normalTexture);
            WriteString(file, layer->maskTexture);
            WriteF32(file, layer->tiling.x);
            WriteF32(file, layer->tiling.y);
            WriteF32(file, layer->offset.x);
            WriteF32(file, layer->offset.y);
            WriteF32(file, layer->opacity);
            WriteF32(file, layer->metallic);
            WriteF32(file, layer->roughness);
            WriteF32(file, layer->normalStrength);
        }

        WriteI32(file, m_currentTerrain->splatmapResolution);
        // Exactly splatRes*splatRes*4 bytes: validated above.
        if (expectedSplatBytes > 0)
        {
            file.write(reinterpret_cast<const char*>(m_currentTerrain->splatmaps.data()),
                       static_cast<std::streamsize>(expectedSplatBytes));
        }

        WriteU32(file, static_cast<uint32_t>(m_currentTerrain->detailMeshes.size()));
        for (size_t i = 0; i < m_currentTerrain->detailMeshes.size(); ++i)
        {
            const auto& mesh = m_currentTerrain->detailMeshes[i];
            WriteString(file, mesh->name);
            WriteString(file, mesh->meshPath);
            WriteString(file, mesh->materialPath);
            WriteF32(file, mesh->density);
            WriteF32(file, mesh->viewDistance);

            const std::vector<XMFLOAT3> empty;
            const std::vector<XMFLOAT3>& instances =
                i < m_currentTerrain->detailInstances.size() ? m_currentTerrain->detailInstances[i] : empty;
            WriteU32(file, static_cast<uint32_t>(instances.size()));
            if (!instances.empty())
            {
                file.write(reinterpret_cast<const char*>(instances.data()),
                           static_cast<std::streamsize>(instances.size() * sizeof(XMFLOAT3)));
            }
        }

        file.flush();
        if (!file.good())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Terrain write failed: %s", filePath.c_str());
            return false;
        }

        m_terrainFilePath = filePath;
        RefreshPathBuffers();
        SetModified(false);

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Terrain saved to: %s (heightmap=%dx%d, layers=%zu, splatmap=%d)",
                       filePath.c_str(), m_currentTerrain->heightmap.width, m_currentTerrain->heightmap.height,
                       m_currentTerrain->textureLayers.size(), m_currentTerrain->splatmapResolution);

        return true;
    }

    void TerrainEditor::PlaceDetailMeshes(int detailIndex, const XMFLOAT4& region, float density)
    {
        if (!m_currentTerrain || detailIndex < 0 ||
            detailIndex >= static_cast<int>(m_currentTerrain->detailMeshes.size()))
            return;

        if (m_currentTerrain->detailInstances.size() <= static_cast<size_t>(detailIndex))
            m_currentTerrain->detailInstances.resize(static_cast<size_t>(detailIndex) + 1);

        auto& instances = m_currentTerrain->detailInstances[static_cast<size_t>(detailIndex)];
        float step = 1.0f / std::max(density, 0.1f);
        for (float x = region.x; x <= region.z; x += step)
            for (float z = region.y; z <= region.w; z += step)
            {
                float h = m_currentTerrain->heightmap.GetHeightInterpolated(x, z, m_currentTerrain->size);
                instances.push_back({x, h, z});
            }
        SetModified(true);
    }

    // --- Tool dispatch ---

    void TerrainEditor::ApplyToolAtPosition(const XMFLOAT3& worldPosition, float strength)
    {
        if (!m_currentTerrain)
            return;

        int toolGroup = static_cast<int>(m_currentTool);
        if (toolGroup < 10)
            ApplySculptingTool(worldPosition, strength);
        else if (toolGroup < 20)
            ApplyTexturePaintingTool(worldPosition, strength);
        else
            ApplyDetailPlacementTool(worldPosition, strength);
    }

    // --- Undo/redo ---

    void TerrainEditor::BeginTerrainOperation(TerrainOperation::Type operationType, const std::string& description)
    {
        m_currentOperation = std::make_unique<TerrainOperation>();
        m_currentOperation->type = operationType;
        m_currentOperation->description = description;
    }

    void TerrainEditor::EndTerrainOperation()
    {
        if (!m_currentOperation)
            return;

        m_undoStack.push_back(std::move(m_currentOperation));
        m_redoStack.clear();

        // Trim undo stack to configured maximum
        while (static_cast<int>(m_undoStack.size()) > m_maxUndoOperations)
        {
            m_undoStack.erase(m_undoStack.begin());
        }
    }

    void TerrainEditor::UndoOperation()
    {
        if (m_undoStack.empty())
            return;
        m_redoStack.push_back(std::move(m_undoStack.back()));
        m_undoStack.pop_back();
    }

    void TerrainEditor::RedoOperation()
    {
        if (m_redoStack.empty())
            return;
        m_undoStack.push_back(std::move(m_redoStack.back()));
        m_redoStack.pop_back();
    }

    // --- Mesh/collision rebuild ---

    void TerrainEditor::UpdateTerrainMesh()
    {
        m_meshDirty = false;
        if (!m_currentTerrain)
            return;

        // Push changes to the engine so the viewport shows the updated terrain
        SyncToEngine();

        auto& hm = m_currentTerrain->heightmap;
        int w = hm.width;
        int h = hm.height;
        if (w < 2 || h < 2 || hm.heights.empty())
            return;

        float terrainSize = m_currentTerrain->size;
        float cellSize = terrainSize / static_cast<float>(w - 1);
        float halfSize = terrainSize * 0.5f;

        // Build vertex positions from heightmap
        size_t vertCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        m_meshVertices.resize(vertCount);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
                m_meshVertices[idx] = {static_cast<float>(x) * cellSize - halfSize + m_currentTerrain->position.x,
                                       hm.GetHeight(x, y) * hm.scale + m_currentTerrain->position.y,
                                       static_cast<float>(y) * cellSize - halfSize + m_currentTerrain->position.z};
            }
        }

        // Build normals via central-difference on the heightmap
        m_meshNormals.resize(vertCount);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                float hL = hm.GetHeight(x - 1, y) * hm.scale;
                float hR = hm.GetHeight(x + 1, y) * hm.scale;
                float hD = hm.GetHeight(x, y - 1) * hm.scale;
                float hU = hm.GetHeight(x, y + 1) * hm.scale;

                XMFLOAT3 n = {(hL - hR) / (2.0f * cellSize), 1.0f, (hD - hU) / (2.0f * cellSize)};
                float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (len > 0.0f)
                {
                    n.x /= len;
                    n.y /= len;
                    n.z /= len;
                }

                size_t idx = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
                m_meshNormals[idx] = n;
            }
        }

        // Build triangle indices — two triangles per heightmap quad
        m_meshIndices.clear();
        m_meshIndices.reserve(static_cast<size_t>(w - 1) * static_cast<size_t>(h - 1) * 6);
        for (int y = 0; y < h - 1; ++y)
        {
            for (int x = 0; x < w - 1; ++x)
            {
                auto tl = static_cast<uint32_t>(y * w + x);
                auto tr = static_cast<uint32_t>(y * w + x + 1);
                auto bl = static_cast<uint32_t>((y + 1) * w + x);
                auto br = static_cast<uint32_t>((y + 1) * w + x + 1);
                m_meshIndices.push_back(tl);
                m_meshIndices.push_back(bl);
                m_meshIndices.push_back(tr);
                m_meshIndices.push_back(tr);
                m_meshIndices.push_back(bl);
                m_meshIndices.push_back(br);
            }
        }
    }

    void TerrainEditor::UpdateTerrainCollision()
    {
        m_collisionDirty = false;
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        if (hm.heights.empty())
            return;

        // Build a contiguous world-space height array for physics heightfield shapes.
        // Bullet Physics uses this directly for btHeightfieldTerrainShape.
        size_t count = hm.heights.size();
        m_collisionHeights.resize(count);
        float posY = m_currentTerrain->position.y;
        for (size_t i = 0; i < count; ++i)
        {
            m_collisionHeights[i] = hm.heights[i] * hm.scale + posY;
        }
    }

    // =========================================================================
    // Engine sync — push editor terrain data to ECS TerrainComponent
    // =========================================================================

    void TerrainEditor::SyncToEngine()
    {
        if (!m_currentTerrain)
            return;

        auto* ctx = EngineContext::Get();
        if (!ctx)
            return;

        auto* world = ctx->GetWorld();
        if (!world)
            return;

        // Find or create a terrain entity in the ECS world
        // Look for an existing entity with TerrainComponent
        entt::entity terrainEntity = entt::null;
        auto& reg = world->GetRegistry();
        auto terrainView = reg.view<TerrainComponent>();
        for (auto entity : terrainView)
        {
            terrainEntity = entity;
            break; // Use the first terrain entity found
        }

        // If no terrain entity exists, create one
        if (terrainEntity == entt::null)
        {
            terrainEntity = reg.create();
            reg.emplace<TerrainComponent>(terrainEntity);
            reg.emplace<Transform>(terrainEntity, Transform{m_currentTerrain->position});
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Created new terrain entity in ECS world");
        }

        // Sync heightmap data from editor to engine component
        auto& tc = reg.get<TerrainComponent>(terrainEntity);
        auto& hm = m_currentTerrain->heightmap;

        tc.heightmapResolution = hm.width;
        tc.heightScale = hm.scale;
        tc.minHeight = hm.minHeight;
        tc.maxHeight = hm.maxHeight;
        tc.terrainSize = m_currentTerrain->size;
        tc.lodLevels = m_meshLODLevels;

        // Copy heightmap data
        tc.heightmap = hm.heights;

        // Copy splatmap data
        tc.splatmap = m_currentTerrain->splatmaps;
        tc.splatmapResolution = m_currentTerrain->splatmapResolution;

        // Copy texture layer diffuse paths. The component field is a texture path, not a display name:
        // pushing layer->name here made the runtime try to resolve textures called "Grass" or "New Layer".
        tc.textureLayerPaths.clear();
        for (const auto& layer : m_currentTerrain->textureLayers)
        {
            tc.textureLayerPaths.push_back(layer->diffuseTexture);
        }

        // Mark dirty so TerrainSystem will push to ClipmapTerrain and rebuild meshes
        tc.dirty = true;

        // Update transform
        auto& transform = reg.get<Transform>(terrainEntity);
        transform.position = m_currentTerrain->position;
    }

} // namespace SparkEditor
