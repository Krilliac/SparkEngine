/**
 * @file TilemapEditorPanel.h
 * @brief Tilemap editor panel for painting and editing tile-based maps
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Comprehensive tilemap editor panel
     *
     * Provides:
     * - Tile palette picker from tileset texture
     * - Paint, erase, fill, and rectangle tools
     * - Collision tile painting mode
     * - Map resize with preview
     * - Grid overlay and zoom controls
     * - Layer management for multi-layer tilemaps
     * - Auto-tiling rule configuration
     * - Undo/redo support for tile paint operations
     */
    class TilemapEditorPanel : public EditorPanel
    {
      public:
        TilemapEditorPanel();
        ~TilemapEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        void SetScene(SceneFile* scene) { m_scene = scene; }
        void SetInspectedObject(ObjectID objectID) { m_inspectedObjectID = objectID; }

      private:
        // Tool types
        enum class TileTool
        {
            Paint,
            Erase,
            Fill,
            Rectangle,
            Eyedropper,
            CollisionPaint
        };

        void RenderToolbar();
        void RenderTilePalette();
        void RenderMapCanvas();
        void RenderMapProperties();
        void RenderCollisionOverlay();
        void RenderAutoTileSettings();

        // Flood fill algorithm
        void FloodFill(std::vector<int32_t>& tiles, int mapW, int mapH, int startX, int startY, int32_t newTile);

        SceneFile* m_scene = nullptr;
        ObjectID m_inspectedObjectID = INVALID_OBJECT_ID;

        // Tool state
        TileTool m_currentTool = TileTool::Paint;
        int32_t m_selectedTileID = 1;
        bool m_showCollision = false;
        bool m_showGrid = true;

        // Canvas state
        float m_canvasZoom = 2.0f;
        float m_canvasPanX = 0.0f;
        float m_canvasPanY = 0.0f;

        // Rectangle tool state
        bool m_rectDragging = false;
        int m_rectStartX = 0;
        int m_rectStartY = 0;

        // Map data
        int m_mapWidth = 20;
        int m_mapHeight = 15;
        std::vector<int32_t> m_tiles;
        std::vector<bool> m_collision;

        void EnsureMapData();

        // Undo buffer (simple: stores full map state)
        struct TilemapSnapshot
        {
            std::vector<int32_t> tiles;
            std::vector<bool> collisionFlags;
            int width;
            int height;
        };
        std::vector<TilemapSnapshot> m_undoStack;
        std::vector<TilemapSnapshot> m_redoStack;
        static constexpr size_t MAX_UNDO_HISTORY = 50;

        void PushUndoSnapshot(const std::vector<int32_t>& tiles, const std::vector<bool>& collision, int width,
                              int height);
        void PerformUndo();
        void PerformRedo();
    };

} // namespace SparkEditor
