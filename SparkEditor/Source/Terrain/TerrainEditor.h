/**
 * @file TerrainEditor.h
 * @brief Terrain editing panel for the Spark Engine Editor
 */

#pragma once

#include "TerrainData.h"
#include <cstdint>
#include "../Core/EditorPanel.h"

namespace SparkEditor
{

    /**
     * @brief Professional terrain editing system with sculpting, painting, and generation tools.
     */
    class TerrainEditor : public EditorPanel
    {
      public:
        TerrainEditor();
        ~TerrainEditor() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        // --- Terrain management ---
        void CreateNewTerrain(float size = 1000.0f, int heightmapResolution = 513,
                              const XMFLOAT3& position = {0, 0, 0});
        bool LoadTerrain(const std::string& filePath);
        bool SaveTerrain(const std::string& filePath);

        // --- Tool interface ---
        void SetCurrentTool(TerrainTool tool) { m_currentTool = tool; }
        TerrainTool GetCurrentTool() const { return m_currentTool; }
        TerrainBrush& GetBrushSettings() { return m_brushSettings; }
        TerrainData* GetCurrentTerrain() const { return m_currentTerrain.get(); }
        void ApplyToolAtPosition(const XMFLOAT3& worldPosition, float strength = 1.0f);

        // --- Undo/redo ---
        void BeginTerrainOperation(TerrainOperation::Type operationType, const std::string& description);
        void EndTerrainOperation();
        void UndoOperation();
        void RedoOperation();

        // --- Generation ---
        void GenerateNoiseHeightmap(int octaves = 6, float frequency = 0.01f, float amplitude = 50.0f,
                                    float lacunarity = 2.0f, float persistence = 0.5f);
        void SmoothTerrain(int iterations = 1, float strength = 0.5f);
        void ApplyErosion(int iterations = 100, float strength = 0.1f, float evaporationRate = 0.02f,
                          float depositionRate = 0.3f);
        void AutoGenerateTexturePlacement(int layerIndex);
        void PlaceDetailMeshes(int detailIndex, const XMFLOAT4& region, float density);

        // --- Mesh/collision rebuild ---
        void UpdateTerrainMesh();
        void UpdateTerrainCollision();

        /**
         * @brief Sync editor terrain data to the engine's ECS TerrainComponent
         *
         * Pushes heightmap, splatmap, and texture layers from the editor's TerrainData
         * to the engine's TerrainComponent on the terrain entity. Call after sculpting,
         * painting, or procedural generation to see changes in real-time in the viewport.
         *
         * If no terrain entity exists in the scene, creates one automatically.
         */
        void SyncToEngine();

      private:
        // UI rendering (TerrainEditorUI.cpp)
        void RenderNoTerrainView();
        void RenderToolPalette();
        void RenderBrushSettings();
        void RenderHeightmapTools();
        void RenderTexturePaintingTools();
        void RenderDetailPlacementTools();
        void RenderTerrainProperties();
        void RenderTextureLayersPanel();
        void RenderDetailMeshesPanel();
        void RenderGenerationTools();

        // Tool application (TerrainEditorTools.cpp)
        void ApplySculptingTool(const XMFLOAT3& worldPosition, float strength);
        void ApplyTexturePaintingTool(const XMFLOAT3& worldPosition, float strength);
        void ApplyDetailPlacementTool(const XMFLOAT3& worldPosition, float strength);
        void ModifyTerrainHeight(int centerX, int centerY, float radius, float heightDelta,
                                 TerrainBrush::FalloffType falloffType);
        void PaintTextureWeight(int centerX, int centerY, float radius, int layerIndex, float strength);
        XMFLOAT3 CalculateTerrainNormal(int x, int y) const;
        float CalculateTerrainSlope(int x, int y) const;
        bool WorldToHeightmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const;
        bool WorldToSplatmapCoords(const XMFLOAT3& worldPosition, int& outX, int& outY) const;
        float GeneratePerlinNoise(float x, float y, float frequency) const;
        void ApplyHydraulicErosion(int x, int y, float strength);

      private:
        std::unique_ptr<TerrainData> m_currentTerrain;
        TerrainTool m_currentTool = TerrainTool::SCULPT_RAISE;
        TerrainBrush m_brushSettings;
        int m_selectedTextureLayer = 0;
        int m_selectedDetailMesh = 0;

        bool m_isApplyingTool = false;
        XMFLOAT3 m_lastToolPosition = {0, 0, 0};
        float m_lastToolTime = 0.0f;

        std::vector<std::unique_ptr<TerrainOperation>> m_undoStack;
        std::vector<std::unique_ptr<TerrainOperation>> m_redoStack;
        int m_maxUndoOperations = 50;
        std::unique_ptr<TerrainOperation> m_currentOperation;

        bool m_showHeightmapTools = true;
        bool m_showTexturePainting = false;
        bool m_showDetailPlacement = false;
        bool m_showGenerationTools = false;
        float m_toolPanelWidth = 250.0f;

        bool m_showWireframe = false;
        bool m_showNormals = false;
        bool m_showSplatmaps = false;
        bool m_showBrushPreview = true;

        int m_meshLODLevels = 4;
        float m_cullDistance = 1000.0f;
        bool m_enableOcclusionCulling = true;

        // CPU-side mesh data rebuilt by UpdateTerrainMesh()
        std::vector<XMFLOAT3> m_meshVertices;
        std::vector<XMFLOAT3> m_meshNormals;
        std::vector<uint32_t> m_meshIndices;
        bool m_meshDirty = false;

        // Collision heightfield rebuilt by UpdateTerrainCollision()
        std::vector<float> m_collisionHeights;
        bool m_collisionDirty = false;

        struct GenerationParams
        {
            int noiseOctaves = 6;
            float noiseFrequency = 0.01f;
            float noiseAmplitude = 50.0f;
            float noiseLacunarity = 2.0f;
            float noisePersistence = 0.5f;
            int erosionIterations = 100;
            float erosionStrength = 0.1f;
            float evaporationRate = 0.02f;
            float depositionRate = 0.3f;
            int smoothIterations = 1;
            float smoothStrength = 0.5f;
        } m_generationParams;
    };

} // namespace SparkEditor
