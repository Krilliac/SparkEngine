// TerrainEditor.cpp — Core terrain editor: lifecycle, load/save, undo/redo, mesh/collision rebuild

#include "TerrainEditor.h"
#include "Utils/Validate.h"
#include "../Core/EditorIcons.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace DirectX;
namespace SparkEditor
{

    TerrainEditor::TerrainEditor() : EditorPanel("Terrain Editor", "terrain_editor") {}
    TerrainEditor::~TerrainEditor() = default;

    bool TerrainEditor::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Terrain Editor panel\n";
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
        UpdateTerrainMesh();
        UpdateTerrainCollision();
        SetModified(true);
    }

    bool TerrainEditor::LoadTerrain(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;

        uint32_t magic = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != 0x53504B54) // 'SPKT'
            return false;

        auto terrain = std::make_unique<TerrainData>();

        uint32_t nameLen = 0;
        file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (nameLen > 4096)
            return false;
        terrain->name.resize(nameLen);
        file.read(terrain->name.data(), nameLen);

        file.read(reinterpret_cast<char*>(&terrain->size), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->position), sizeof(XMFLOAT3));
        file.read(reinterpret_cast<char*>(&terrain->lodLevels), sizeof(int));
        file.read(reinterpret_cast<char*>(&terrain->lodBias), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->generateCollider), sizeof(bool));

        file.read(reinterpret_cast<char*>(&terrain->heightmap.width), sizeof(int));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.height), sizeof(int));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.scale), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.minHeight), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.maxHeight), sizeof(float));

        size_t heightCount = static_cast<size_t>(terrain->heightmap.width) * terrain->heightmap.height;
        terrain->heightmap.heights.resize(heightCount);
        file.read(reinterpret_cast<char*>(terrain->heightmap.heights.data()),
                  static_cast<std::streamsize>(heightCount * sizeof(float)));

        uint32_t layerCount = 0;
        file.read(reinterpret_cast<char*>(&layerCount), sizeof(layerCount));
        for (uint32_t i = 0; i < layerCount && i < 64; ++i)
        {
            uint32_t lnameLen = 0;
            file.read(reinterpret_cast<char*>(&lnameLen), sizeof(lnameLen));
            if (lnameLen > 4096)
                break;
            std::string lname(lnameLen, '\0');
            file.read(lname.data(), lnameLen);
            terrain->AddTextureLayer(lname);
        }

        file.read(reinterpret_cast<char*>(&terrain->splatmapResolution), sizeof(int));
        size_t splatSize = static_cast<size_t>(terrain->splatmapResolution) * terrain->splatmapResolution * 4;
        terrain->splatmaps.resize(splatSize);
        if (splatSize > 0)
        {
            file.read(reinterpret_cast<char*>(terrain->splatmaps.data()), static_cast<std::streamsize>(splatSize));
        }

        if (!file.good())
            return false;

        m_currentTerrain = std::move(terrain);
        m_undoStack.clear();
        m_redoStack.clear();
        UpdateTerrainMesh();
        UpdateTerrainCollision();
        SetModified(false);
        return true;
    }

    bool TerrainEditor::SaveTerrain(const std::string& filePath)
    {
        if (!m_currentTerrain)
            return false;

        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;

        uint32_t magic = 0x53504B54; // 'SPKT'
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

        auto nameLen = static_cast<uint32_t>(m_currentTerrain->name.size());
        file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        file.write(m_currentTerrain->name.data(), nameLen);

        file.write(reinterpret_cast<const char*>(&m_currentTerrain->size), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->position), sizeof(XMFLOAT3));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->lodLevels), sizeof(int));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->lodBias), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->generateCollider), sizeof(bool));

        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.width), sizeof(int));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.height), sizeof(int));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.scale), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.minHeight), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.maxHeight), sizeof(float));

        size_t heightCount = m_currentTerrain->heightmap.heights.size();
        file.write(reinterpret_cast<const char*>(m_currentTerrain->heightmap.heights.data()),
                   static_cast<std::streamsize>(heightCount * sizeof(float)));

        auto layerCount = static_cast<uint32_t>(m_currentTerrain->textureLayers.size());
        file.write(reinterpret_cast<const char*>(&layerCount), sizeof(layerCount));
        for (const auto& layer : m_currentTerrain->textureLayers)
        {
            auto lnameLen = static_cast<uint32_t>(layer->name.size());
            file.write(reinterpret_cast<const char*>(&lnameLen), sizeof(lnameLen));
            file.write(layer->name.data(), lnameLen);
        }

        file.write(reinterpret_cast<const char*>(&m_currentTerrain->splatmapResolution), sizeof(int));
        if (!m_currentTerrain->splatmaps.empty())
        {
            file.write(reinterpret_cast<const char*>(m_currentTerrain->splatmaps.data()),
                       static_cast<std::streamsize>(m_currentTerrain->splatmaps.size()));
        }

        SetModified(false);
        return file.good();
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
        while (static_cast<int>(m_undoStack.size()) > m_maxUndoOperations)
            m_undoStack.erase(m_undoStack.begin());
    }

    void TerrainEditor::UndoOperation()
    {
        if (!m_undoStack.empty())
        {
            m_redoStack.push_back(std::move(m_undoStack.back()));
            m_undoStack.pop_back();
        }
    }

    void TerrainEditor::RedoOperation()
    {
        if (!m_redoStack.empty())
        {
            m_undoStack.push_back(std::move(m_redoStack.back()));
            m_redoStack.pop_back();
        }
    }

    // --- Mesh/collision rebuild ---

    void TerrainEditor::UpdateTerrainMesh()
    {
        m_meshDirty = false;
        if (!m_currentTerrain)
            return;
        auto& hm = m_currentTerrain->heightmap;
        int w = hm.width, h = hm.height;
        if (w < 2 || h < 2 || hm.heights.empty())
            return;

        float cellSize = m_currentTerrain->size / static_cast<float>(w - 1);
        float halfSize = m_currentTerrain->size * 0.5f;
        auto idx = [w](int x, int y) { return static_cast<size_t>(y) * static_cast<size_t>(w) + x; };

        // Build vertex positions
        size_t vertCount = static_cast<size_t>(w) * h;
        m_meshVertices.resize(vertCount);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                m_meshVertices[idx(x, y)] = {x * cellSize - halfSize + m_currentTerrain->position.x,
                                             hm.GetHeight(x, y) * hm.scale + m_currentTerrain->position.y,
                                             y * cellSize - halfSize + m_currentTerrain->position.z};

        // Build normals from heightmap finite differences
        m_meshNormals.resize(vertCount);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                XMFLOAT3 n = {(hm.GetHeight(x - 1, y) - hm.GetHeight(x + 1, y)) * hm.scale / (2.0f * cellSize),
                              1.0f,
                              (hm.GetHeight(x, y - 1) - hm.GetHeight(x, y + 1)) * hm.scale / (2.0f * cellSize)};
                float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (len > 0.0f)
                {
                    n.x /= len;
                    n.y /= len;
                    n.z /= len;
                }
                m_meshNormals[idx(x, y)] = n;
            }

        // Build triangle indices (two triangles per quad)
        m_meshIndices.clear();
        m_meshIndices.reserve(static_cast<size_t>(w - 1) * (h - 1) * 6);
        for (int y = 0; y < h - 1; ++y)
            for (int x = 0; x < w - 1; ++x)
            {
                auto tl = static_cast<uint32_t>(y * w + x);
                auto tr = tl + 1, bl = static_cast<uint32_t>((y + 1) * w + x), br = bl + 1;
                m_meshIndices.insert(m_meshIndices.end(), {tl, bl, tr, tr, bl, br});
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
        // Build world-space height array for physics heightfield shapes (Bullet)
        m_collisionHeights.resize(hm.heights.size());
        float posY = m_currentTerrain->position.y;
        for (size_t i = 0; i < hm.heights.size(); ++i)
            m_collisionHeights[i] = hm.heights[i] * hm.scale + posY;
    }

} // namespace SparkEditor
