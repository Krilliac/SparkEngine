/**
 * @file TerrainData.h
 * @brief Data structures for the terrain editing system
 */

#pragma once

#include "../SceneSystem/SceneFile.h"
#include <cstdint>
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace SparkEditor
{

    /// Terrain editing tool types
    enum class TerrainTool
    {
        SCULPT_RAISE = 0,
        SCULPT_LOWER = 1,
        SCULPT_SMOOTH = 2,
        SCULPT_FLATTEN = 3,
        SCULPT_NOISE = 4,
        SCULPT_EROSION = 5,

        PAINT_TEXTURE = 10,
        PAINT_DETAIL = 11,
        PAINT_TREES = 12,

        STAMP_HEIGHT = 20,
        STAMP_TEXTURE = 21,

        ROAD_TOOL = 30,
        RIVER_TOOL = 31,
        PLATEAU_TOOL = 32,

        MEASURE = 40,
        SAMPLE = 41
    };

    /// Terrain brush settings
    struct TerrainBrush
    {
        float radius = 50.0f;
        float strength = 0.5f;
        float falloff = 0.5f;
        float spacing = 0.1f;

        enum FalloffType
        {
            LINEAR = 0,
            SMOOTH = 1,
            SPHERE = 2,
            SHARP = 3,
            CUSTOM = 4
        } falloffType = SMOOTH;

        bool enablePressure = false;
        bool enableJitter = false;
        float jitterAmount = 0.1f;

        bool showPreview = true;
        XMFLOAT4 previewColor = {1, 1, 0, 0.5f};

        float EvaluateFalloff(float distance) const;
    };

    /// Terrain heightmap data
    struct TerrainHeightmap
    {
        int width = 513;
        int height = 513;
        float scale = 1.0f;
        float minHeight = 0.0f;
        float maxHeight = 100.0f;
        std::vector<float> heights;

        float GetHeight(int x, int y) const;
        void SetHeight(int x, int y, float height);
        float GetHeightInterpolated(float worldX, float worldZ, float terrainSize) const;
        void Resize(int newWidth, int newHeight, bool preserveData = true);
        void Generate(std::function<float(int, int)> generator);
        bool LoadFromImage(const std::string& filePath);
        bool SaveToImage(const std::string& filePath) const;
    };

    /// Terrain texture layer
    struct TerrainTextureLayer
    {
        std::string name = "Layer";
        std::string diffuseTexture;
        std::string normalTexture;
        std::string maskTexture;
        XMFLOAT2 tiling = {1, 1};
        XMFLOAT2 offset = {0, 0};
        float opacity = 1.0f;
        float metallic = 0.0f;
        float roughness = 0.5f;
        float normalStrength = 1.0f;
        bool useAutoPlacement = false;
        float minHeight = 0.0f;
        float maxHeight = 100.0f;
        float minSlope = 0.0f;
        float maxSlope = 90.0f;
        float placementStrength = 1.0f;
        bool isVisible = true;
        bool isLocked = false;
    };

    /// Terrain detail mesh (grass, rocks, etc.)
    struct TerrainDetailMesh
    {
        std::string name = "Detail";
        std::string meshPath;
        std::string materialPath;
        float density = 1.0f;
        XMFLOAT2 scaleRange = {0.8f, 1.2f};
        XMFLOAT2 rotationRange = {0, 360};
        float viewDistance = 100.0f;
        int maxInstancesPerCell = 100;
        float minHeight = 0.0f;
        float maxHeight = 100.0f;
        float minSlope = 0.0f;
        float maxSlope = 45.0f;
        bool isVisible = true;
        bool castShadows = true;
        bool receiveShadows = true;
    };

    /// Terrain data structure
    struct TerrainData
    {
        std::string name = "Terrain";
        float size = 1000.0f;
        XMFLOAT3 position = {0, 0, 0};
        TerrainHeightmap heightmap;
        std::vector<std::unique_ptr<TerrainTextureLayer>> textureLayers;
        std::vector<uint8_t> splatmaps;
        int splatmapResolution = 512;
        std::vector<std::unique_ptr<TerrainDetailMesh>> detailMeshes;
        std::vector<std::vector<XMFLOAT3>> detailInstances;
        bool generateCollider = true;
        std::string physicsMaterial;
        int lodLevels = 4;
        float lodBias = 1.0f;

        TerrainTextureLayer* GetTextureLayer(int index);
        TerrainTextureLayer* AddTextureLayer(const std::string& name = "New Layer");
        void RemoveTextureLayer(int index);
        uint8_t GetSplatmapWeight(int x, int y, int layer) const;
        void SetSplatmapWeight(int x, int y, int layer, uint8_t weight);
    };

    /// Terrain operation for undo/redo
    struct TerrainOperation
    {
        enum Type
        {
            HEIGHT_MODIFICATION,
            TEXTURE_PAINTING,
            DETAIL_PLACEMENT
        } type;

        std::vector<uint8_t> undoData;
        std::vector<uint8_t> redoData;
        std::string description;
        XMFLOAT4 affectedRegion;
    };

} // namespace SparkEditor
