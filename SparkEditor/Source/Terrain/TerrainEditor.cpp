/**
 * @file TerrainEditor.cpp
 * @brief Implementation of the terrain editing system
 * @author Spark Engine Team
 * @date 2025
 */

#include "TerrainEditor.h"
#include "Utils/Validate.h"
#include "../Core/EditorIcons.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>

using namespace DirectX;
namespace SparkEditor
{

    // --- TerrainBrush ---

    float TerrainBrush::EvaluateFalloff(float distance) const
    {
        float d = std::clamp(distance, 0.0f, 1.0f);
        switch (falloffType)
        {
        case LINEAR:
            return 1.0f - d;
        case SMOOTH:
            return 1.0f - (3.0f * d * d - 2.0f * d * d * d); // smoothstep
        case SPHERE:
            return std::sqrt(std::max(0.0f, 1.0f - d * d));
        case SHARP:
            return (1.0f - d) * (1.0f - d);
        case CUSTOM:
        default:
            return 1.0f - d;
        }
    }

    // --- TerrainHeightmap ---

    float TerrainHeightmap::GetHeight(int x, int y) const
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return 0.0f;
        if (heights.empty())
            return 0.0f;
        return heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
    }

    void TerrainHeightmap::SetHeight(int x, int y, float h)
    {
        if (x < 0 || x >= width || y < 0 || y >= height)
            return;
        if (heights.empty())
            return;
        heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = h;
    }

    float TerrainHeightmap::GetHeightInterpolated(float worldX, float worldZ, float terrainSize) const
    {
        if (heights.empty() || terrainSize <= 0.0f)
            return 0.0f;

        float u = (worldX / terrainSize + 0.5f) * static_cast<float>(width - 1);
        float v = (worldZ / terrainSize + 0.5f) * static_cast<float>(height - 1);

        int x0 = static_cast<int>(std::floor(u));
        int y0 = static_cast<int>(std::floor(v));
        int x1 = std::min(x0 + 1, width - 1);
        int y1 = std::min(y0 + 1, height - 1);
        x0 = std::clamp(x0, 0, width - 1);
        y0 = std::clamp(y0, 0, height - 1);

        float fx = u - std::floor(u);
        float fy = v - std::floor(v);

        float h00 = GetHeight(x0, y0);
        float h10 = GetHeight(x1, y0);
        float h01 = GetHeight(x0, y1);
        float h11 = GetHeight(x1, y1);

        float top = h00 + (h10 - h00) * fx;
        float bot = h01 + (h11 - h01) * fx;
        return (top + (bot - top) * fy) * scale;
    }

    void TerrainHeightmap::Resize(int newWidth, int newHeight, bool preserveData)
    {
        if (newWidth <= 0 || newHeight <= 0)
            return;

        std::vector<float> newHeights(static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight), 0.0f);

        if (preserveData && !heights.empty())
        {
            int copyW = std::min(width, newWidth);
            int copyH = std::min(height, newHeight);
            for (int y = 0; y < copyH; ++y)
            {
                for (int x = 0; x < copyW; ++x)
                {
                    newHeights[static_cast<size_t>(y) * static_cast<size_t>(newWidth) + static_cast<size_t>(x)] =
                        heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
                }
            }
        }

        width = newWidth;
        height = newHeight;
        heights = std::move(newHeights);
    }

    void TerrainHeightmap::Generate(std::function<float(int, int)> generator)
    {
        if (!generator)
            return;
        heights.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                heights[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] = generator(x, y);
            }
        }
    }

    bool TerrainHeightmap::LoadFromImage(const std::string& filePath)
    {
        // Load raw 16-bit or 8-bit heightmap (RAW format: width*height samples, no header)
        std::ifstream file(filePath, std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return false;

        auto fileSize = static_cast<size_t>(file.tellg());
        file.seekg(0);

        // Determine dimensions: assume square heightmap
        size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

        bool is16Bit = (fileSize >= pixelCount * 2);
        bool is8Bit = (fileSize >= pixelCount);
        if (!is16Bit && !is8Bit)
            return false;

        heights.resize(pixelCount);
        if (is16Bit)
        {
            std::vector<uint16_t> raw(pixelCount);
            file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(pixelCount * 2));
            for (size_t i = 0; i < pixelCount; ++i)
                heights[i] = minHeight + (static_cast<float>(raw[i]) / 65535.0f) * (maxHeight - minHeight);
        }
        else
        {
            std::vector<uint8_t> raw(pixelCount);
            file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(pixelCount));
            for (size_t i = 0; i < pixelCount; ++i)
                heights[i] = minHeight + (static_cast<float>(raw[i]) / 255.0f) * (maxHeight - minHeight);
        }
        return file.good();
    }

    bool TerrainHeightmap::SaveToImage(const std::string& filePath) const
    {
        // Save as 16-bit RAW heightmap
        if (heights.empty())
            return false;

        std::ofstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;

        float range = maxHeight - minHeight;
        if (range < 1e-6f)
            range = 1.0f;

        std::vector<uint16_t> raw(heights.size());
        for (size_t i = 0; i < heights.size(); ++i)
        {
            float normalized = std::clamp((heights[i] - minHeight) / range, 0.0f, 1.0f);
            raw[i] = static_cast<uint16_t>(normalized * 65535.0f);
        }
        file.write(reinterpret_cast<const char*>(raw.data()),
                   static_cast<std::streamsize>(raw.size() * sizeof(uint16_t)));
        return file.good();
    }

    // --- TerrainData ---

    TerrainTextureLayer* TerrainData::GetTextureLayer(int index)
    {
        if (index < 0 || index >= static_cast<int>(textureLayers.size()))
            return nullptr;
        return textureLayers[static_cast<size_t>(index)].get();
    }

    TerrainTextureLayer* TerrainData::AddTextureLayer(const std::string& name)
    {
        auto layer = std::make_unique<TerrainTextureLayer>();
        layer->name = name;
        auto* ptr = layer.get();
        textureLayers.push_back(std::move(layer));
        return ptr;
    }

    void TerrainData::RemoveTextureLayer(int index)
    {
        if (index < 0 || index >= static_cast<int>(textureLayers.size()))
            return;
        textureLayers.erase(textureLayers.begin() + index);
    }

    uint8_t TerrainData::GetSplatmapWeight(int x, int y, int layer) const
    {
        if (layer < 0 || layer >= 4)
            return 0;
        int idx = (y * splatmapResolution + x) * 4 + layer;
        if (idx < 0 || idx >= static_cast<int>(splatmaps.size()))
            return 0;
        return splatmaps[static_cast<size_t>(idx)];
    }

    void TerrainData::SetSplatmapWeight(int x, int y, int layer, uint8_t weight)
    {
        if (layer < 0 || layer >= 4)
            return;
        int idx = (y * splatmapResolution + x) * 4 + layer;
        if (idx < 0 || idx >= static_cast<int>(splatmaps.size()))
            return;
        splatmaps[static_cast<size_t>(idx)] = weight;
    }

    // --- TerrainEditor ---

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

        // Initialize splatmap
        m_currentTerrain->splatmaps.resize(static_cast<size_t>(m_currentTerrain->splatmapResolution) *
                                               static_cast<size_t>(m_currentTerrain->splatmapResolution) * 4,
                                           0);

        // Default base texture layer
        m_currentTerrain->AddTextureLayer("Grass");

        m_undoStack.clear();
        m_redoStack.clear();
        SetModified(true);
    }

    bool TerrainEditor::LoadTerrain(const std::string& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open())
            return false;

        // Magic number check
        uint32_t magic = 0;
        file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        if (magic != 0x53504B54) // 'SPKT'
            return false;

        auto terrain = std::make_unique<TerrainData>();

        // Read terrain name
        uint32_t nameLen = 0;
        file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        if (nameLen > 4096)
            return false;
        terrain->name.resize(nameLen);
        file.read(terrain->name.data(), nameLen);

        // Read basic properties
        file.read(reinterpret_cast<char*>(&terrain->size), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->position), sizeof(XMFLOAT3));
        file.read(reinterpret_cast<char*>(&terrain->lodLevels), sizeof(int));
        file.read(reinterpret_cast<char*>(&terrain->lodBias), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->generateCollider), sizeof(bool));

        // Read heightmap
        file.read(reinterpret_cast<char*>(&terrain->heightmap.width), sizeof(int));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.height), sizeof(int));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.scale), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.minHeight), sizeof(float));
        file.read(reinterpret_cast<char*>(&terrain->heightmap.maxHeight), sizeof(float));

        size_t heightCount = static_cast<size_t>(terrain->heightmap.width) * terrain->heightmap.height;
        terrain->heightmap.heights.resize(heightCount);
        file.read(reinterpret_cast<char*>(terrain->heightmap.heights.data()),
                  static_cast<std::streamsize>(heightCount * sizeof(float)));

        // Read texture layer count
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

        // Read splatmap
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

        // Magic number
        uint32_t magic = 0x53504B54; // 'SPKT'
        file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));

        // Terrain name
        auto nameLen = static_cast<uint32_t>(m_currentTerrain->name.size());
        file.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        file.write(m_currentTerrain->name.data(), nameLen);

        // Basic properties
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->size), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->position), sizeof(XMFLOAT3));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->lodLevels), sizeof(int));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->lodBias), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->generateCollider), sizeof(bool));

        // Heightmap
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.width), sizeof(int));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.height), sizeof(int));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.scale), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.minHeight), sizeof(float));
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->heightmap.maxHeight), sizeof(float));

        size_t heightCount = m_currentTerrain->heightmap.heights.size();
        file.write(reinterpret_cast<const char*>(m_currentTerrain->heightmap.heights.data()),
                   static_cast<std::streamsize>(heightCount * sizeof(float)));

        // Texture layers
        auto layerCount = static_cast<uint32_t>(m_currentTerrain->textureLayers.size());
        file.write(reinterpret_cast<const char*>(&layerCount), sizeof(layerCount));
        for (const auto& layer : m_currentTerrain->textureLayers)
        {
            auto lnameLen = static_cast<uint32_t>(layer->name.size());
            file.write(reinterpret_cast<const char*>(&lnameLen), sizeof(lnameLen));
            file.write(layer->name.data(), lnameLen);
        }

        // Splatmap
        file.write(reinterpret_cast<const char*>(&m_currentTerrain->splatmapResolution), sizeof(int));
        if (!m_currentTerrain->splatmaps.empty())
        {
            file.write(reinterpret_cast<const char*>(m_currentTerrain->splatmaps.data()),
                       static_cast<std::streamsize>(m_currentTerrain->splatmaps.size()));
        }

        SetModified(false);
        return file.good();
    }

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

        // Trim undo stack
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

    void TerrainEditor::GenerateNoiseHeightmap(int octaves, float frequency, float amplitude, float lacunarity,
                                               float persistence)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        hm.heights.resize(static_cast<size_t>(hm.width) * static_cast<size_t>(hm.height));

        for (int y = 0; y < hm.height; ++y)
        {
            for (int x = 0; x < hm.width; ++x)
            {
                float h = 0.0f;
                float freq = frequency;
                float amp = amplitude;
                for (int o = 0; o < octaves; ++o)
                {
                    h += GeneratePerlinNoise(static_cast<float>(x), static_cast<float>(y), freq) * amp;
                    freq *= lacunarity;
                    amp *= persistence;
                }
                hm.SetHeight(x, y, h);
            }
        }
        SetModified(true);
    }

    void TerrainEditor::SmoothTerrain(int iterations, float strength)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        for (int iter = 0; iter < iterations; ++iter)
        {
            std::vector<float> smoothed = hm.heights;
            for (int y = 1; y < hm.height - 1; ++y)
            {
                for (int x = 1; x < hm.width - 1; ++x)
                {
                    float avg = (hm.GetHeight(x - 1, y) + hm.GetHeight(x + 1, y) + hm.GetHeight(x, y - 1) +
                                 hm.GetHeight(x, y + 1)) *
                                0.25f;
                    float current = hm.GetHeight(x, y);
                    smoothed[static_cast<size_t>(y) * static_cast<size_t>(hm.width) + static_cast<size_t>(x)] =
                        current + (avg - current) * strength;
                }
            }
            hm.heights = std::move(smoothed);
        }
        SetModified(true);
    }

    void TerrainEditor::ApplyErosion(int iterations, float strength, float evaporationRate, float depositionRate)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        for (int i = 0; i < iterations; ++i)
        {
            // Simple thermal erosion: move height from steep to low neighbors
            int x = std::rand() % hm.width;
            int y = std::rand() % hm.height;
            ApplyHydraulicErosion(x, y, strength);
        }
        (void)evaporationRate;
        (void)depositionRate;
        SetModified(true);
    }

    void TerrainEditor::AutoGenerateTexturePlacement(int layerIndex)
    {
        if (!m_currentTerrain || layerIndex < 0 ||
            layerIndex >= static_cast<int>(m_currentTerrain->textureLayers.size()))
            return;

        auto* layer = m_currentTerrain->textureLayers[static_cast<size_t>(layerIndex)].get();
        if (!layer->useAutoPlacement)
            return;

        int res = m_currentTerrain->splatmapResolution;
        for (int y = 0; y < res; ++y)
        {
            for (int x = 0; x < res; ++x)
            {
                float hmX = static_cast<float>(x) / static_cast<float>(res) *
                            static_cast<float>(m_currentTerrain->heightmap.width - 1);
                float hmY = static_cast<float>(y) / static_cast<float>(res) *
                            static_cast<float>(m_currentTerrain->heightmap.height - 1);

                float h = m_currentTerrain->heightmap.GetHeight(static_cast<int>(hmX), static_cast<int>(hmY));
                float slope = CalculateTerrainSlope(static_cast<int>(hmX), static_cast<int>(hmY));

                bool inRange = h >= layer->minHeight && h <= layer->maxHeight && slope >= layer->minSlope &&
                               slope <= layer->maxSlope;

                if (inRange && layerIndex < 4)
                {
                    auto w = static_cast<uint8_t>(layer->placementStrength * 255.0f);
                    m_currentTerrain->SetSplatmapWeight(x, y, layerIndex, w);
                }
            }
        }
        SetModified(true);
    }

    void TerrainEditor::PlaceDetailMeshes(int detailIndex, const XMFLOAT4& region, float density)
    {
        if (!m_currentTerrain || detailIndex < 0 ||
            detailIndex >= static_cast<int>(m_currentTerrain->detailMeshes.size()))
            return;

        // Ensure instance vector exists
        if (m_currentTerrain->detailInstances.size() <= static_cast<size_t>(detailIndex))
            m_currentTerrain->detailInstances.resize(static_cast<size_t>(detailIndex) + 1);

        auto& instances = m_currentTerrain->detailInstances[static_cast<size_t>(detailIndex)];
        float step = 1.0f / std::max(density, 0.1f);

        for (float x = region.x; x <= region.z; x += step)
        {
            for (float z = region.y; z <= region.w; z += step)
            {
                float h = m_currentTerrain->heightmap.GetHeightInterpolated(x, z, m_currentTerrain->size);
                instances.push_back({x, h, z});
            }
        }
        SetModified(true);
    }

    void TerrainEditor::UpdateTerrainMesh()
    {
        // GPU mesh rebuild would happen here — requires graphics context
    }

    void TerrainEditor::UpdateTerrainCollision()
    {
        // Physics collision mesh rebuild would happen here — requires physics context
    }

    // --- Private: UI Rendering ---

    void TerrainEditor::RenderNoTerrainView()
    {
        ImGui::Spacing();
        ImGui::TextWrapped("No terrain loaded. Create a new terrain or load an existing one.");
        ImGui::Spacing();

        if (ImGui::Button(ICON_FA_PLUS " Create New Terrain", ImVec2(-1, 32)))
        {
            CreateNewTerrain();
        }
    }

    void TerrainEditor::RenderToolPalette()
    {
        ImGui::Text(ICON_FA_MOUNTAIN " Terrain Tools");
        ImGui::Spacing();

        float btnWidth = (ImGui::GetContentRegionAvail().x - 12.0f) / 4.0f;
        ImVec2 btnSize(btnWidth, 28.0f);

        auto ToolButton = [&](const char* label, TerrainTool tool)
        {
            bool active = (m_currentTool == tool);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.9f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.95f, 1.0f));
            }
            if (ImGui::Button(label, btnSize))
            {
                m_currentTool = tool;
                m_showHeightmapTools = (static_cast<int>(tool) < 10);
                m_showTexturePainting = (static_cast<int>(tool) >= 10 && static_cast<int>(tool) < 20);
                m_showDetailPlacement = (static_cast<int>(tool) >= 10 && static_cast<int>(tool) < 20 &&
                                         (tool == TerrainTool::PAINT_DETAIL || tool == TerrainTool::PAINT_TREES));
                m_showGenerationTools = false;
            }
            if (active)
                ImGui::PopStyleColor(2);
            ImGui::SameLine();
        };

        // Sculpt row
        ImGui::TextDisabled("Sculpt:");
        ToolButton(ICON_FA_LEVEL_UP_ALT " Raise", TerrainTool::SCULPT_RAISE);
        ToolButton(ICON_FA_ANGLE_DOWN " Lower", TerrainTool::SCULPT_LOWER);
        ToolButton(ICON_FA_MAGIC " Smooth", TerrainTool::SCULPT_SMOOTH);
        ToolButton(ICON_FA_COMPRESS " Flatten", TerrainTool::SCULPT_FLATTEN);
        ImGui::NewLine();

        // Paint row
        ImGui::TextDisabled("Paint:");
        ToolButton(ICON_FA_PAINT_BRUSH " Texture", TerrainTool::PAINT_TEXTURE);
        ToolButton(ICON_FA_TREE " Trees", TerrainTool::PAINT_TREES);
        ToolButton(ICON_FA_LAYER_GROUP " Detail", TerrainTool::PAINT_DETAIL);
        ImGui::NewLine();

        // Utility row
        ImGui::TextDisabled("Utility:");
        if (ImGui::Button(ICON_FA_COGS " Generate", btnSize))
        {
            m_showGenerationTools = !m_showGenerationTools;
            m_showHeightmapTools = false;
            m_showTexturePainting = false;
            m_showDetailPlacement = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_UNDO " Undo", btnSize))
            UndoOperation();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_REDO " Redo", btnSize))
            RedoOperation();
        ImGui::NewLine();
    }

    void TerrainEditor::RenderBrushSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_PAINT_BRUSH " Brush Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);

            ImGui::DragFloat("Radius", &m_brushSettings.radius, 0.5f, 1.0f, 500.0f, "%.1f");
            ImGui::DragFloat("Strength", &m_brushSettings.strength, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Falloff", &m_brushSettings.falloff, 0.01f, 0.0f, 1.0f, "%.2f");

            const char* falloffTypes[] = {"Linear", "Smooth", "Sphere", "Sharp", "Custom"};
            int falloffIdx = static_cast<int>(m_brushSettings.falloffType);
            if (ImGui::Combo("Falloff Type", &falloffIdx, falloffTypes, 5))
            {
                m_brushSettings.falloffType = static_cast<TerrainBrush::FalloffType>(falloffIdx);
            }

            ImGui::Checkbox("Show Preview", &m_brushSettings.showPreview);
            ImGui::Checkbox("Pen Pressure", &m_brushSettings.enablePressure);

            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderHeightmapTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_MOUNTAIN " Heightmap", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);

            if (m_currentTerrain)
            {
                auto& hm = m_currentTerrain->heightmap;
                ImGui::Text("Resolution: %d x %d", hm.width, hm.height);
                ImGui::DragFloat("Height Scale", &hm.scale, 0.1f, 0.1f, 100.0f, "%.1f");
                ImGui::DragFloat("Min Height", &hm.minHeight, 0.5f, -1000.0f, hm.maxHeight, "%.1f");
                ImGui::DragFloat("Max Height", &hm.maxHeight, 0.5f, hm.minHeight, 1000.0f, "%.1f");

                ImGui::Separator();
                ImGui::Text("Visualization:");
                ImGui::Checkbox("Wireframe", &m_showWireframe);
                ImGui::SameLine();
                ImGui::Checkbox("Normals", &m_showNormals);
            }

            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderTexturePaintingTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_PAINT_BRUSH " Texture Painting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);
            RenderTextureLayersPanel();
            ImGui::Checkbox("Show Splatmaps", &m_showSplatmaps);
            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderDetailPlacementTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_TREE " Detail Placement", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);
            RenderDetailMeshesPanel();
            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderTerrainProperties()
    {
        if (ImGui::CollapsingHeader(ICON_FA_COG " Terrain Properties"))
        {
            if (!m_currentTerrain)
                return;

            ImGui::Indent(8.0f);

            char nameBuffer[256] = {};
            std::strncpy(nameBuffer, m_currentTerrain->name.c_str(), sizeof(nameBuffer) - 1);
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
            {
                m_currentTerrain->name = nameBuffer;
                SetModified(true);
            }

            ImGui::DragFloat("Size", &m_currentTerrain->size, 10.0f, 100.0f, 10000.0f, "%.0f m");
            ImGui::DragFloat3("Position", &m_currentTerrain->position.x, 1.0f);

            ImGui::Separator();
            ImGui::Text("LOD:");
            ImGui::DragInt("LOD Levels", &m_currentTerrain->lodLevels, 0.1f, 1, 8);
            ImGui::DragFloat("LOD Bias", &m_currentTerrain->lodBias, 0.1f, 0.1f, 4.0f, "%.1f");

            ImGui::Separator();
            ImGui::Text("Physics:");
            ImGui::Checkbox("Generate Collider", &m_currentTerrain->generateCollider);

            ImGui::Separator();
            if (ImGui::Button(ICON_FA_SAVE " Save Terrain", ImVec2(-1, 28)))
            {
                SaveTerrain("terrain.sparkterrain");
            }

            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderTextureLayersPanel()
    {
        if (!m_currentTerrain)
            return;

        ImGui::Text("Texture Layers (%d)", static_cast<int>(m_currentTerrain->textureLayers.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Layer"))
        {
            m_currentTerrain->AddTextureLayer("New Layer");
            SetModified(true);
        }

        ImGui::BeginChild("##TextureLayers", ImVec2(0, 150), true);
        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(m_currentTerrain->textureLayers.size()); ++i)
        {
            auto& layer = m_currentTerrain->textureLayers[static_cast<size_t>(i)];
            ImGui::PushID(i);

            bool selected = (m_selectedTextureLayer == i);
            if (ImGui::Selectable(layer->name.c_str(), selected))
            {
                m_selectedTextureLayer = i;
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Tiling: %.1f x %.1f\nOpacity: %.0f%%", layer->tiling.x, layer->tiling.y,
                                  layer->opacity * 100.0f);
            }

            // Context menu
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Remove"))
                    removeIdx = i;
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndChild();

        if (removeIdx >= 0)
        {
            m_currentTerrain->RemoveTextureLayer(removeIdx);
            SetModified(true);
        }

        // Selected layer properties
        if (m_selectedTextureLayer >= 0 &&
            m_selectedTextureLayer < static_cast<int>(m_currentTerrain->textureLayers.size()))
        {
            auto& layer = m_currentTerrain->textureLayers[static_cast<size_t>(m_selectedTextureLayer)];
            ImGui::Separator();
            ImGui::DragFloat2("Tiling", &layer->tiling.x, 0.1f, 0.01f, 100.0f);
            ImGui::DragFloat("Opacity", &layer->opacity, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Metallic", &layer->metallic, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Roughness", &layer->roughness, 0.01f, 0.0f, 1.0f, "%.2f");

            ImGui::Separator();
            ImGui::Checkbox("Auto Placement", &layer->useAutoPlacement);
            if (layer->useAutoPlacement)
            {
                ImGui::DragFloatRange2("Height", &layer->minHeight, &layer->maxHeight, 0.5f, 0.0f, 1000.0f);
                ImGui::DragFloatRange2("Slope", &layer->minSlope, &layer->maxSlope, 0.5f, 0.0f, 90.0f);
                if (ImGui::Button("Apply Auto-Placement"))
                {
                    AutoGenerateTexturePlacement(m_selectedTextureLayer);
                }
            }
        }
    }

    void TerrainEditor::RenderDetailMeshesPanel()
    {
        if (!m_currentTerrain)
            return;

        ImGui::Text("Detail Meshes (%d)", static_cast<int>(m_currentTerrain->detailMeshes.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Detail"))
        {
            auto detail = std::make_unique<TerrainDetailMesh>();
            detail->name = "New Detail";
            m_currentTerrain->detailMeshes.push_back(std::move(detail));
            SetModified(true);
        }

        ImGui::BeginChild("##DetailMeshes", ImVec2(0, 120), true);
        for (int i = 0; i < static_cast<int>(m_currentTerrain->detailMeshes.size()); ++i)
        {
            auto& detail = m_currentTerrain->detailMeshes[static_cast<size_t>(i)];
            ImGui::PushID(i);
            bool selected = (m_selectedDetailMesh == i);
            if (ImGui::Selectable(detail->name.c_str(), selected))
            {
                m_selectedDetailMesh = i;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Selected detail properties
        if (m_selectedDetailMesh >= 0 && m_selectedDetailMesh < static_cast<int>(m_currentTerrain->detailMeshes.size()))
        {
            auto& detail = m_currentTerrain->detailMeshes[static_cast<size_t>(m_selectedDetailMesh)];
            ImGui::DragFloat("Density", &detail->density, 0.1f, 0.1f, 10.0f);
            ImGui::DragFloat("View Distance", &detail->viewDistance, 1.0f, 10.0f, 500.0f);
            ImGui::DragFloat2("Scale Range", &detail->scaleRange.x, 0.05f, 0.1f, 5.0f);
            ImGui::Checkbox("Cast Shadows", &detail->castShadows);
        }
    }

    void TerrainEditor::RenderGenerationTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_COGS " Procedural Generation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);

            ImGui::TextDisabled("Noise");
            ImGui::DragInt("Octaves", &m_generationParams.noiseOctaves, 0.1f, 1, 12);
            ImGui::DragFloat("Frequency", &m_generationParams.noiseFrequency, 0.001f, 0.001f, 1.0f, "%.3f");
            ImGui::DragFloat("Amplitude", &m_generationParams.noiseAmplitude, 1.0f, 1.0f, 500.0f, "%.0f");
            ImGui::DragFloat("Lacunarity", &m_generationParams.noiseLacunarity, 0.1f, 1.0f, 4.0f, "%.1f");
            ImGui::DragFloat("Persistence", &m_generationParams.noisePersistence, 0.01f, 0.0f, 1.0f, "%.2f");

            if (ImGui::Button(ICON_FA_BOLT " Generate Heightmap", ImVec2(-1, 28)))
            {
                GenerateNoiseHeightmap(m_generationParams.noiseOctaves, m_generationParams.noiseFrequency,
                                       m_generationParams.noiseAmplitude, m_generationParams.noiseLacunarity,
                                       m_generationParams.noisePersistence);
            }

            ImGui::Separator();
            ImGui::TextDisabled("Post-Process");

            ImGui::DragInt("Smooth Iterations", &m_generationParams.smoothIterations, 0.1f, 1, 20);
            ImGui::DragFloat("Smooth Strength", &m_generationParams.smoothStrength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button(ICON_FA_MAGIC " Smooth", ImVec2(-1, 28)))
            {
                SmoothTerrain(m_generationParams.smoothIterations, m_generationParams.smoothStrength);
            }

            ImGui::Separator();
            ImGui::DragInt("Erosion Iterations", &m_generationParams.erosionIterations, 1.0f, 10, 10000);
            ImGui::DragFloat("Erosion Strength", &m_generationParams.erosionStrength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button(ICON_FA_FIRE " Apply Erosion", ImVec2(-1, 28)))
            {
                ApplyErosion(m_generationParams.erosionIterations, m_generationParams.erosionStrength,
                             m_generationParams.evaporationRate, m_generationParams.depositionRate);
            }

            ImGui::Unindent(8.0f);
        }
    }

    // --- Private: Tool Application ---

    void TerrainEditor::ApplySculptingTool(const XMFLOAT3& worldPosition, float strength)
    {
        if (!m_currentTerrain)
            return;

        int hx = 0, hy = 0;
        if (!WorldToHeightmapCoords(worldPosition, hx, hy))
            return;

        float brushRadius =
            m_brushSettings.radius / m_currentTerrain->size * static_cast<float>(m_currentTerrain->heightmap.width);
        float delta = m_brushSettings.strength * strength;

        switch (m_currentTool)
        {
        case TerrainTool::SCULPT_RAISE:
            ModifyTerrainHeight(hx, hy, brushRadius, delta, m_brushSettings.falloffType);
            break;
        case TerrainTool::SCULPT_LOWER:
            ModifyTerrainHeight(hx, hy, brushRadius, -delta, m_brushSettings.falloffType);
            break;
        case TerrainTool::SCULPT_SMOOTH:
        {
            int r = static_cast<int>(brushRadius);
            for (int dy = -r; dy <= r; ++dy)
            {
                for (int dx = -r; dx <= r; ++dx)
                {
                    float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / brushRadius;
                    if (dist > 1.0f)
                        continue;

                    int px = hx + dx, py = hy + dy;
                    float avg = (m_currentTerrain->heightmap.GetHeight(px - 1, py) +
                                 m_currentTerrain->heightmap.GetHeight(px + 1, py) +
                                 m_currentTerrain->heightmap.GetHeight(px, py - 1) +
                                 m_currentTerrain->heightmap.GetHeight(px, py + 1)) *
                                0.25f;
                    float current = m_currentTerrain->heightmap.GetHeight(px, py);
                    float falloff = m_brushSettings.EvaluateFalloff(dist);
                    float newH = current + (avg - current) * strength * falloff;
                    m_currentTerrain->heightmap.SetHeight(px, py, newH);
                }
            }
            break;
        }
        case TerrainTool::SCULPT_FLATTEN:
        {
            float targetHeight = m_currentTerrain->heightmap.GetHeight(hx, hy);
            int r = static_cast<int>(brushRadius);
            for (int dy = -r; dy <= r; ++dy)
            {
                for (int dx = -r; dx <= r; ++dx)
                {
                    float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / brushRadius;
                    if (dist > 1.0f)
                        continue;
                    float falloff = m_brushSettings.EvaluateFalloff(dist);
                    float current = m_currentTerrain->heightmap.GetHeight(hx + dx, hy + dy);
                    float newH = current + (targetHeight - current) * falloff * strength;
                    m_currentTerrain->heightmap.SetHeight(hx + dx, hy + dy, newH);
                }
            }
            break;
        }
        default:
            break;
        }
        SetModified(true);
    }

    void TerrainEditor::ApplyTexturePaintingTool(const XMFLOAT3& worldPosition, float strength)
    {
        if (!m_currentTerrain || m_selectedTextureLayer < 0 || m_selectedTextureLayer >= 4)
            return;

        int sx = 0, sy = 0;
        if (!WorldToSplatmapCoords(worldPosition, sx, sy))
            return;

        float brushRadius =
            m_brushSettings.radius / m_currentTerrain->size * static_cast<float>(m_currentTerrain->splatmapResolution);
        int r = static_cast<int>(brushRadius);

        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / brushRadius;
                if (dist > 1.0f)
                    continue;
                float falloff = m_brushSettings.EvaluateFalloff(dist);
                PaintTextureWeight(sx + dx, sy + dy, 1.0f, m_selectedTextureLayer, strength * falloff);
            }
        }
        SetModified(true);
    }

    void TerrainEditor::ApplyDetailPlacementTool(const XMFLOAT3& worldPosition, float /*strength*/)
    {
        if (!m_currentTerrain || m_selectedDetailMesh < 0 ||
            m_selectedDetailMesh >= static_cast<int>(m_currentTerrain->detailMeshes.size()))
            return;

        XMFLOAT4 region = {worldPosition.x - m_brushSettings.radius, worldPosition.z - m_brushSettings.radius,
                           worldPosition.x + m_brushSettings.radius, worldPosition.z + m_brushSettings.radius};
        float density = m_currentTerrain->detailMeshes[static_cast<size_t>(m_selectedDetailMesh)]->density;
        PlaceDetailMeshes(m_selectedDetailMesh, region, density);
    }

    void TerrainEditor::ModifyTerrainHeight(int centerX, int centerY, float radius, float heightDelta,
                                            TerrainBrush::FalloffType falloffType)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        int r = static_cast<int>(radius);

        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                int px = centerX + dx;
                int py = centerY + dy;
                if (px < 0 || px >= hm.width || py < 0 || py >= hm.height)
                    continue;

                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / radius;
                if (dist > 1.0f)
                    continue;

                // Temporarily swap falloff type for evaluation
                TerrainBrush tempBrush;
                tempBrush.falloffType = falloffType;
                float falloff = tempBrush.EvaluateFalloff(dist);

                float current = hm.GetHeight(px, py);
                hm.SetHeight(px, py, current + heightDelta * falloff);
            }
        }
    }

    void TerrainEditor::PaintTextureWeight(int centerX, int centerY, float /*radius*/, int layerIndex, float strength)
    {
        if (!m_currentTerrain || layerIndex < 0 || layerIndex >= 4)
            return;

        int res = m_currentTerrain->splatmapResolution;
        if (centerX < 0 || centerX >= res || centerY < 0 || centerY >= res)
            return;

        uint8_t current = m_currentTerrain->GetSplatmapWeight(centerX, centerY, layerIndex);
        auto newVal = static_cast<uint8_t>(std::min(255.0f, static_cast<float>(current) + strength * 255.0f));
        m_currentTerrain->SetSplatmapWeight(centerX, centerY, layerIndex, newVal);
    }

    XMFLOAT3 TerrainEditor::CalculateTerrainNormal(int x, int y) const
    {
        if (!m_currentTerrain)
            return {0.0f, 1.0f, 0.0f};

        auto& hm = m_currentTerrain->heightmap;
        float hL = hm.GetHeight(x - 1, y);
        float hR = hm.GetHeight(x + 1, y);
        float hD = hm.GetHeight(x, y - 1);
        float hU = hm.GetHeight(x, y + 1);

        float cellSize = m_currentTerrain->size / static_cast<float>(hm.width);
        XMFLOAT3 normal = {(hL - hR) / (2.0f * cellSize), 1.0f, (hD - hU) / (2.0f * cellSize)};

        // Normalize
        float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (len > 0.0f)
        {
            normal.x /= len;
            normal.y /= len;
            normal.z /= len;
        }
        return normal;
    }

    float TerrainEditor::CalculateTerrainSlope(int x, int y) const
    {
        XMFLOAT3 normal = CalculateTerrainNormal(x, y);
        // Slope = angle between normal and up vector (0,1,0)
        float dot = normal.y; // dot(normal, up) where up = (0,1,0)
        return std::acos(std::clamp(dot, -1.0f, 1.0f)) * (180.0f / 3.14159265f);
    }

    bool TerrainEditor::WorldToHeightmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const
    {
        if (!m_currentTerrain)
            return false;

        float relX = worldPosition.x - m_currentTerrain->position.x;
        float relZ = worldPosition.z - m_currentTerrain->position.z;
        float halfSize = m_currentTerrain->size * 0.5f;

        outX = static_cast<int>((relX + halfSize) / m_currentTerrain->size *
                                static_cast<float>(m_currentTerrain->heightmap.width - 1));
        outY = static_cast<int>((relZ + halfSize) / m_currentTerrain->size *
                                static_cast<float>(m_currentTerrain->heightmap.height - 1));

        return outX >= 0 && outX < m_currentTerrain->heightmap.width && outY >= 0 &&
               outY < m_currentTerrain->heightmap.height;
    }

    bool TerrainEditor::WorldToSplatmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const
    {
        if (!m_currentTerrain)
            return false;

        float relX = worldPosition.x - m_currentTerrain->position.x;
        float relZ = worldPosition.z - m_currentTerrain->position.z;
        float halfSize = m_currentTerrain->size * 0.5f;

        int res = m_currentTerrain->splatmapResolution;
        outX = static_cast<int>((relX + halfSize) / m_currentTerrain->size * static_cast<float>(res - 1));
        outY = static_cast<int>((relZ + halfSize) / m_currentTerrain->size * static_cast<float>(res - 1));

        return outX >= 0 && outX < res && outY >= 0 && outY < res;
    }

    float TerrainEditor::GeneratePerlinNoise(float x, float y, float frequency) const
    {
        // Simple value noise — adequate for terrain generation preview
        float fx = x * frequency;
        float fy = y * frequency;

        int ix = static_cast<int>(std::floor(fx));
        int iy = static_cast<int>(std::floor(fy));
        float tx = fx - std::floor(fx);
        float ty = fy - std::floor(fy);

        // Smoothstep interpolation
        tx = tx * tx * (3.0f - 2.0f * tx);
        ty = ty * ty * (3.0f - 2.0f * ty);

        // Hash-based pseudo-random corners
        auto hash = [](int xi, int yi) -> float
        {
            int n = xi * 73856093 ^ yi * 19349663;
            n = (n << 13) ^ n;
            return 1.0f - static_cast<float>((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f;
        };

        float c00 = hash(ix, iy);
        float c10 = hash(ix + 1, iy);
        float c01 = hash(ix, iy + 1);
        float c11 = hash(ix + 1, iy + 1);

        float top = c00 + (c10 - c00) * tx;
        float bot = c01 + (c11 - c01) * tx;
        return top + (bot - top) * ty;
    }

    void TerrainEditor::ApplyHydraulicErosion(int x, int y, float strength)
    {
        if (!m_currentTerrain)
            return;

        auto& hm = m_currentTerrain->heightmap;
        if (x <= 0 || x >= hm.width - 1 || y <= 0 || y >= hm.height - 1)
            return;

        float center = hm.GetHeight(x, y);

        // Find lowest neighbor
        float minH = center;
        int minDx = 0, minDy = 0;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0)
                    continue;
                float h = hm.GetHeight(x + dx, y + dy);
                if (h < minH)
                {
                    minH = h;
                    minDx = dx;
                    minDy = dy;
                }
            }
        }

        float diff = center - minH;
        if (diff > 0.001f)
        {
            float transfer = diff * strength * 0.5f;
            hm.SetHeight(x, y, center - transfer);
            hm.SetHeight(x + minDx, y + minDy, minH + transfer);
        }
    }

} // namespace SparkEditor
