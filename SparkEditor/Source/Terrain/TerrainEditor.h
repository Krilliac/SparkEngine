/**
 * @file TerrainEditor.h
 * @brief Advanced terrain editing system for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a comprehensive terrain editing system with height-based
 * sculpting, texture painting, vegetation placement, and advanced terrain tools
 * similar to World Machine, Unity Terrain Tools, and Unreal Landscape system.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <DirectXMath.h>
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>


namespace SparkEditor {

/**
 * @brief Terrain editing tool types
 */
enum class TerrainTool {
    SCULPT_RAISE = 0,           ///< Raise terrain height
    SCULPT_LOWER = 1,           ///< Lower terrain height
    SCULPT_SMOOTH = 2,          ///< Smooth terrain surface
    SCULPT_FLATTEN = 3,         ///< Flatten terrain to target height
    SCULPT_NOISE = 4,           ///< Add noise to terrain
    SCULPT_EROSION = 5,         ///< Apply erosion effects
    
    PAINT_TEXTURE = 10,         ///< Paint textures on terrain
    PAINT_DETAIL = 11,          ///< Paint detail meshes/grass
    PAINT_TREES = 12,           ///< Paint trees and large vegetation
    
    STAMP_HEIGHT = 20,          ///< Stamp height patterns
    STAMP_TEXTURE = 21,         ///< Stamp texture patterns
    
    ROAD_TOOL = 30,             ///< Create roads and paths
    RIVER_TOOL = 31,            ///< Create rivers and waterways
    PLATEAU_TOOL = 32,          ///< Create flat plateaus
    
    MEASURE = 40,               ///< Measure distances and heights
    SAMPLE = 41                 ///< Sample terrain properties
};

/**
 * @brief Terrain brush settings
 */
struct TerrainBrush {
    float radius = 50.0f;                       ///< Brush radius in world units
    float strength = 0.5f;                      ///< Brush strength (0-1)
    float falloff = 0.5f;                       ///< Brush falloff (0-1)
    float spacing = 0.1f;                       ///< Brush spacing for continuous painting
    
    enum FalloffType {
        LINEAR = 0,
        SMOOTH = 1,
        SPHERE = 2,
        SHARP = 3,
        CUSTOM = 4
    } falloffType = SMOOTH;                     ///< Falloff curve type
    
    bool enablePressure = false;                ///< Use pen pressure (if available)
    bool enableJitter = false;                  ///< Enable brush jitter
    float jitterAmount = 0.1f;                  ///< Jitter intensity
    
    // Visualization
    bool showPreview = true;                    ///< Show brush preview
    XMFLOAT4 previewColor = {1, 1, 0, 0.5f};   ///< Brush preview color
    
    /**
     * @brief Evaluate brush falloff at distance
     * @param distance Distance from brush center (normalized 0-1)
     * @return Falloff value (0-1)
     */
    float EvaluateFalloff(float distance) const;
};

/**
 * @brief Terrain heightmap data
 */
struct TerrainHeightmap {
    int width = 513;                            ///< Heightmap width in samples
    int height = 513;                           ///< Heightmap height in samples
    float scale = 1.0f;                         ///< Height scale multiplier
    float minHeight = 0.0f;                     ///< Minimum terrain height
    float maxHeight = 100.0f;                   ///< Maximum terrain height
    std::vector<float> heights;                 ///< Height values (width * height)
    
    /**
     * @brief Get height at position
     * @param x X coordinate (0 to width-1)
     * @param y Y coordinate (0 to height-1)
     * @return Height value
     */
    float GetHeight(int x, int y) const;
    
    /**
     * @brief Set height at position
     * @param x X coordinate (0 to width-1)
     * @param y Y coordinate (0 to height-1)
     * @param height New height value
     */
    void SetHeight(int x, int y, float height);
    
    /**
     * @brief Get interpolated height at world position
     * @param worldX World X coordinate
     * @param worldZ World Z coordinate
     * @param terrainSize Terrain size in world units
     * @return Interpolated height
     */
    float GetHeightInterpolated(float worldX, float worldZ, float terrainSize) const;
    
    /**
     * @brief Resize heightmap
     * @param newWidth New width
     * @param newHeight New height
     * @param preserveData Whether to preserve existing data
     */
    void Resize(int newWidth, int newHeight, bool preserveData = true);
    
    /**
     * @brief Generate heightmap from function
     * @param generator Function that takes (x, y) and returns height
     */
    void Generate(std::function<float(int, int)> generator);
    
    /**
     * @brief Load heightmap from image file
     * @param filePath Path to heightmap image
     * @return true if loaded successfully
     */
    bool LoadFromImage(const std::string& filePath);
    
    /**
     * @brief Save heightmap to image file
     * @param filePath Path to save heightmap
     * @return true if saved successfully
     */
    bool SaveToImage(const std::string& filePath) const;
};

/**
 * @brief Terrain texture layer
 */
struct TerrainTextureLayer {
    std::string name = "Layer";                 ///< Layer display name
    std::string diffuseTexture;                 ///< Diffuse texture path
    std::string normalTexture;                  ///< Normal map texture path
    std::string maskTexture;                    ///< Layer mask texture path
    
    // Tiling and offset
    XMFLOAT2 tiling = {1, 1};                  ///< Texture tiling
    XMFLOAT2 offset = {0, 0};                  ///< Texture offset
    
    // Blending properties
    float opacity = 1.0f;                       ///< Layer opacity
    float metallic = 0.0f;                      ///< Metallic value
    float roughness = 0.5f;                     ///< Roughness value
    float normalStrength = 1.0f;                ///< Normal map strength
    
    // Auto-placement rules
    bool useAutoPlacement = false;              ///< Use automatic placement
    float minHeight = 0.0f;                     ///< Minimum height for placement
    float maxHeight = 100.0f;                   ///< Maximum height for placement
    float minSlope = 0.0f;                      ///< Minimum slope for placement (degrees)
    float maxSlope = 90.0f;                     ///< Maximum slope for placement (degrees)
    float placementStrength = 1.0f;             ///< Auto-placement strength
    
    bool isVisible = true;                      ///< Whether layer is visible
    bool isLocked = false;                      ///< Whether layer is locked
};

/**
 * @brief Terrain detail mesh (grass, rocks, etc.)
 */
struct TerrainDetailMesh {
    std::string name = "Detail";                ///< Detail display name
    std::string meshPath;                       ///< Mesh asset path
    std::string materialPath;                   ///< Material asset path
    
    // Placement properties
    float density = 1.0f;                       ///< Placement density
    XMFLOAT2 scaleRange = {0.8f, 1.2f};        ///< Random scale range
    XMFLOAT2 rotationRange = {0, 360};         ///< Random rotation range (degrees)
    
    // LOD settings
    float viewDistance = 100.0f;                ///< Maximum view distance
    int maxInstancesPerCell = 100;              ///< Maximum instances per cell
    
    // Placement constraints
    float minHeight = 0.0f;                     ///< Minimum height for placement
    float maxHeight = 100.0f;                   ///< Maximum height for placement
    float minSlope = 0.0f;                      ///< Minimum slope for placement
    float maxSlope = 45.0f;                     ///< Maximum slope for placement
    
    bool isVisible = true;                      ///< Whether detail is visible
    bool castShadows = true;                    ///< Whether detail casts shadows
    bool receiveShadows = true;                 ///< Whether detail receives shadows
};

/**
 * @brief Terrain data structure
 */
struct TerrainData {
    // Basic properties
    std::string name = "Terrain";               ///< Terrain name
    float size = 1000.0f;                       ///< Terrain size in world units (square)
    XMFLOAT3 position = {0, 0, 0};             ///< Terrain world position
    
    // Heightmap
    TerrainHeightmap heightmap;                 ///< Terrain heightmap data
    
    // Texture layers
    std::vector<std::unique_ptr<TerrainTextureLayer>> textureLayers; ///< Texture layers
    std::vector<uint8_t> splatmaps;             ///< Texture blend weights (RGBA per pixel)
    int splatmapResolution = 512;               ///< Splatmap resolution
    
    // Detail meshes
    std::vector<std::unique_ptr<TerrainDetailMesh>> detailMeshes; ///< Detail mesh definitions
    std::vector<std::vector<XMFLOAT3>> detailInstances; ///< Detail mesh instances
    
    // Physics properties
    bool generateCollider = true;               ///< Generate physics collider
    std::string physicsMaterial;                ///< Physics material path
    
    // LOD settings
    int lodLevels = 4;                          ///< Number of LOD levels
    float lodBias = 1.0f;                       ///< LOD bias multiplier
    
    /**
     * @brief Get texture layer by index
     * @param index Layer index
     * @return Pointer to layer, or nullptr if invalid
     */
    TerrainTextureLayer* GetTextureLayer(int index);
    
    /**
     * @brief Add new texture layer
     * @param name Layer name
     * @return Pointer to created layer
     */
    TerrainTextureLayer* AddTextureLayer(const std::string& name = "New Layer");
    
    /**
     * @brief Remove texture layer
     * @param index Layer index to remove
     */
    void RemoveTextureLayer(int index);
    
    /**
     * @brief Get splatmap weight at position
     * @param x X coordinate (0 to splatmapResolution-1)
     * @param y Y coordinate (0 to splatmapResolution-1)
     * @param layer Layer index (0-3)
     * @return Weight value (0-255)
     */
    uint8_t GetSplatmapWeight(int x, int y, int layer) const;
    
    /**
     * @brief Set splatmap weight at position
     * @param x X coordinate (0 to splatmapResolution-1)
     * @param y Y coordinate (0 to splatmapResolution-1)
     * @param layer Layer index (0-3)
     * @param weight Weight value (0-255)
     */
    void SetSplatmapWeight(int x, int y, int layer, uint8_t weight);
};

/**
 * @brief Terrain operation for undo/redo
 */
struct TerrainOperation {
    enum Type {
        HEIGHT_MODIFICATION,
        TEXTURE_PAINTING,
        DETAIL_PLACEMENT
    } type;
    
    std::vector<uint8_t> undoData;              ///< Data for undo operation
    std::vector<uint8_t> redoData;              ///< Data for redo operation
    std::string description;                    ///< Operation description
    XMFLOAT4 affectedRegion;                    ///< Affected region (min_x, min_y, max_x, max_y)
};

/**
 * @brief Professional terrain editing system
 * 
 * Provides comprehensive terrain editing tools including:
 * - Height-based sculpting with multiple brush types
 * - Multi-layer texture painting with blend modes
 * - Detail mesh and vegetation placement
 * - Procedural generation tools
 * - Erosion and weathering simulation
 * - Road and river creation tools
 * - Real-time preview and visualization
 * - Performance optimization with LOD
 * - Import/export functionality
 * - Undo/redo system
 * 
 * Inspired by World Machine, Unity Terrain Tools, Unreal Landscape, and CryEngine Terrain.
 */
class TerrainEditor : public EditorPanel {
public:
    /**
     * @brief Constructor
     */
    TerrainEditor();

    /**
     * @brief Destructor
     */
    ~TerrainEditor() override;

    /**
     * @brief Initialize the terrain editor
     * @return true if initialization succeeded
     */
    bool Initialize() override;

    /**
     * @brief Update terrain editor
     * @param deltaTime Time elapsed since last update
     */
    void Update(float deltaTime) override;

    /**
     * @brief Render terrain editor UI
     */
    void Render() override;

    /**
     * @brief Shutdown the terrain editor
     */
    void Shutdown() override;

    /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
    bool HandleEvent(const std::string& eventType, void* eventData) override;

    /**
     * @brief Create new terrain
     * @param size Terrain size in world units
     * @param heightmapResolution Heightmap resolution
     * @param position Terrain world position
     */
    void CreateNewTerrain(float size = 1000.0f, int heightmapResolution = 513, 
                         const XMFLOAT3& position = {0, 0, 0});

    /**
     * @brief Load terrain from file
     * @param filePath Path to terrain file
     * @return true if terrain was loaded successfully
     */
    bool LoadTerrain(const std::string& filePath);

    /**
     * @brief Save current terrain to file
     * @param filePath Path to save terrain
     * @return true if terrain was saved successfully
     */
    bool SaveTerrain(const std::string& filePath);

    /**
     * @brief Set current terrain tool
     * @param tool Terrain tool to activate
     */
    void SetCurrentTool(TerrainTool tool) { m_currentTool = tool; }

    /**
     * @brief Get current terrain tool
     * @return Current terrain tool
     */
    TerrainTool GetCurrentTool() const { return m_currentTool; }

    /**
     * @brief Get terrain brush settings
     * @return Reference to brush settings
     */
    TerrainBrush& GetBrushSettings() { return m_brushSettings; }

    /**
     * @brief Get current terrain data
     * @return Pointer to current terrain, or nullptr if none
     */
    TerrainData* GetCurrentTerrain() const { return m_currentTerrain.get(); }

    /**
     * @brief Apply terrain tool at position
     * @param worldPosition World position to apply tool
     * @param strength Tool strength multiplier
     */
    void ApplyToolAtPosition(const XMFLOAT3& worldPosition, float strength = 1.0f);

    /**
     * @brief Start terrain operation (for undo/redo)
     * @param operationType Type of operation
     * @param description Operation description
     */
    void BeginTerrainOperation(TerrainOperation::Type operationType, const std::string& description);

    /**
     * @brief End terrain operation
     */
    void EndTerrainOperation();

    /**
     * @brief Undo last terrain operation
     */
    void UndoOperation();

    /**
     * @brief Redo last undone operation
     */
    void RedoOperation();

    /**
     * @brief Generate terrain using noise
     * @param octaves Number of noise octaves
     * @param frequency Base frequency
     * @param amplitude Base amplitude
     * @param lacunarity Lacunarity (frequency multiplier)
     * @param persistence Persistence (amplitude multiplier)
     */
    void GenerateNoiseHeightmap(int octaves = 6, float frequency = 0.01f, 
                               float amplitude = 50.0f, float lacunarity = 2.0f, 
                               float persistence = 0.5f);

    /**
     * @brief Smooth entire terrain
     * @param iterations Number of smoothing iterations
     * @param strength Smoothing strength
     */
    void SmoothTerrain(int iterations = 1, float strength = 0.5f);

    /**
     * @brief Apply erosion to terrain
     * @param iterations Number of erosion iterations
     * @param strength Erosion strength
     * @param evaporationRate Water evaporation rate
     * @param depositionRate Sediment deposition rate
     */
    void ApplyErosion(int iterations = 100, float strength = 0.1f, 
                     float evaporationRate = 0.02f, float depositionRate = 0.3f);

    /**
     * @brief Auto-generate texture placement
     * @param layerIndex Texture layer index
     */
    void AutoGenerateTexturePlacement(int layerIndex);

    /**
     * @brief Place detail mesh instances in region
     * @param detailIndex Detail mesh index
     * @param region Region to place instances
     * @param density Placement density
     */
    void PlaceDetailMeshes(int detailIndex, const XMFLOAT4& region, float density);

    /**
     * @brief Update terrain mesh for rendering
     */
    void UpdateTerrainMesh();

    /**
     * @brief Update terrain collision
     */
    void UpdateTerrainCollision();

private:
    /**
     * @brief Render tool palette
     */
    void RenderToolPalette();

    /**
     * @brief Render brush settings
     */
    void RenderBrushSettings();

    /**
     * @brief Render heightmap tools
     */
    void RenderHeightmapTools();

    /**
     * @brief Render texture painting tools
     */
    void RenderTexturePaintingTools();

    /**
     * @brief Render detail placement tools
     */
    void RenderDetailPlacementTools();

    /**
     * @brief Render terrain properties
     */
    void RenderTerrainProperties();

    /**
     * @brief Render texture layers panel
     */
    void RenderTextureLayersPanel();

    /**
     * @brief Render detail meshes panel
     */
    void RenderDetailMeshesPanel();

    /**
     * @brief Render terrain generation tools
     */
    void RenderGenerationTools();

    /**
     * @brief Apply sculpting tool
     * @param worldPosition Tool application position
     * @param strength Tool strength
     */
    void ApplySculptingTool(const XMFLOAT3& worldPosition, float strength);

    /**
     * @brief Apply texture painting tool
     * @param worldPosition Tool application position
     * @param strength Tool strength
     */
    void ApplyTexturePaintingTool(const XMFLOAT3& worldPosition, float strength);

    /**
     * @brief Apply detail placement tool
     * @param worldPosition Tool application position
     * @param strength Tool strength
     */
    void ApplyDetailPlacementTool(const XMFLOAT3& worldPosition, float strength);

    /**
     * @brief Modify terrain height in region
     * @param centerX Center X coordinate
     * @param centerY Center Y coordinate
     * @param radius Modification radius
     * @param heightDelta Height change
     * @param falloffType Falloff curve type
     */
    void ModifyTerrainHeight(int centerX, int centerY, float radius, 
                           float heightDelta, TerrainBrush::FalloffType falloffType);

    /**
     * @brief Paint texture weight in region
     * @param centerX Center X coordinate
     * @param centerY Center Y coordinate
     * @param radius Paint radius
     * @param layerIndex Texture layer index
     * @param strength Paint strength
     */
    void PaintTextureWeight(int centerX, int centerY, float radius, 
                          int layerIndex, float strength);

    /**
     * @brief Calculate terrain normal at position
     * @param x X coordinate
     * @param y Y coordinate
     * @return Terrain normal vector
     */
    XMFLOAT3 CalculateTerrainNormal(int x, int y) const;

    /**
     * @brief Calculate terrain slope at position
     * @param x X coordinate
     * @param y Y coordinate
     * @return Slope angle in degrees
     */
    float CalculateTerrainSlope(int x, int y) const;

    /**
     * @brief World position to heightmap coordinates
     * @param worldPosition World position
     * @param outX Output X coordinate
     * @param outY Output Y coordinate
     * @return true if position is within terrain bounds
     */
    bool WorldToHeightmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const;

    /**
     * @brief World position to splatmap coordinates
     * @param worldPosition World position
     * @param outX Output X coordinate
     * @param outY Output Y coordinate
     * @return true if position is within terrain bounds
     */
    bool WorldToSplatmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const;

    /**
     * @brief Generate Perlin noise
     * @param x X coordinate
     * @param y Y coordinate
     * @param frequency Noise frequency
     * @return Noise value (-1 to 1)
     */
    float GeneratePerlinNoise(float x, float y, float frequency) const;

    /**
     * @brief Apply hydraulic erosion at position
     * @param x X coordinate
     * @param y Y coordinate
     * @param strength Erosion strength
     */
    void ApplyHydraulicErosion(int x, int y, float strength);

private:
    // Current terrain data
    std::unique_ptr<TerrainData> m_currentTerrain;  ///< Current terrain being edited
    
    // Tool settings
    TerrainTool m_currentTool = TerrainTool::SCULPT_RAISE; ///< Current terrain tool
    TerrainBrush m_brushSettings;                   ///< Current brush settings
    int m_selectedTextureLayer = 0;                 ///< Selected texture layer
    int m_selectedDetailMesh = 0;                   ///< Selected detail mesh
    
    // Interaction state
    bool m_isApplyingTool = false;                  ///< Currently applying tool
    XMFLOAT3 m_lastToolPosition = {0, 0, 0};       ///< Last tool application position
    float m_lastToolTime = 0.0f;                   ///< Last tool application time
    
    // Undo/redo system
    std::vector<std::unique_ptr<TerrainOperation>> m_undoStack; ///< Undo operation stack
    std::vector<std::unique_ptr<TerrainOperation>> m_redoStack; ///< Redo operation stack
    int m_maxUndoOperations = 50;                   ///< Maximum undo operations
    std::unique_ptr<TerrainOperation> m_currentOperation; ///< Current operation being recorded
    
    // UI state
    bool m_showHeightmapTools = true;               ///< Show heightmap tools panel
    bool m_showTexturePainting = false;             ///< Show texture painting panel
    bool m_showDetailPlacement = false;             ///< Show detail placement panel
    bool m_showGenerationTools = false;             ///< Show generation tools panel
    float m_toolPanelWidth = 250.0f;               ///< Tool panel width
    
    // Preview and visualization
    bool m_showWireframe = false;                   ///< Show terrain wireframe
    bool m_showNormals = false;                     ///< Show terrain normals
    bool m_showSplatmaps = false;                   ///< Show texture splatmaps
    bool m_showBrushPreview = true;                 ///< Show brush preview
    
    // Performance settings
    int m_meshLODLevels = 4;                        ///< Number of mesh LOD levels
    float m_cullDistance = 1000.0f;                ///< Terrain cull distance
    bool m_enableOcclusionCulling = true;          ///< Enable occlusion culling
    
    // Generation parameters
    struct GenerationParams {
        // Noise parameters
        int noiseOctaves = 6;
        float noiseFrequency = 0.01f;
        float noiseAmplitude = 50.0f;
        float noiseLacunarity = 2.0f;
        float noisePersistence = 0.5f;
        
        // Erosion parameters
        int erosionIterations = 100;
        float erosionStrength = 0.1f;
        float evaporationRate = 0.02f;
        float depositionRate = 0.3f;
        
        // Smoothing parameters
        int smoothIterations = 1;
        float smoothStrength = 0.5f;
    } m_generationParams;
};

} // namespace SparkEditor