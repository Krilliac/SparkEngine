/**
 * @file ObjectPlacementPanel.h
 * @brief Object placement and snapping tools panel for level editing
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    class ObjectPlacementPanel : public EditorPanel
    {
      public:
        enum class PlacementMode
        {
            Single,
            Brush,
            Line,
            Grid,
            Scatter
        };

        enum class AlignMode
        {
            None,
            Surface,
            Grid,
            Vertex
        };

        struct PlacementBrush
        {
            float radius = 5.0f;
            float density = 1.0f;
            float minScale = 0.8f;
            float maxScale = 1.2f;
            float minRotation = 0.0f;
            float maxRotation = 360.0f;
            bool randomizeRotation = true;
            bool randomizeScale = true;
            bool alignToSurface = true;
        };

        struct PrefabEntry
        {
            std::string name;
            std::string path;
            std::string category;
            bool isFavorite = false;
        };

        ObjectPlacementPanel();
        ~ObjectPlacementPanel() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        PlacementMode GetPlacementMode() const { return m_placementMode; }
        AlignMode GetAlignMode() const { return m_alignMode; }
        const PlacementBrush& GetBrush() const { return m_brush; }

      private:
        void RenderPlacementModeSelector();
        void RenderSnapSettings();
        void RenderBrushSettings();
        void RenderPrefabLibrary();
        void RenderTransformConstraints();
        void RenderQuickPlace();

        PlacementMode m_placementMode = PlacementMode::Single;
        AlignMode m_alignMode = AlignMode::Grid;
        PlacementBrush m_brush;

        // Snapping
        bool m_snapToGrid = true;
        float m_snapGridSize = 1.0f;
        bool m_snapRotation = true;
        float m_snapRotationAngle = 15.0f;
        bool m_snapScale = false;
        float m_snapScaleIncrement = 0.25f;
        bool m_snapToSurface = false;
        bool m_snapToVertex = false;

        // Transform constraints
        bool m_lockX = false;
        bool m_lockY = false;
        bool m_lockZ = false;
        bool m_uniformScale = true;

        // Prefab library
        std::vector<PrefabEntry> m_prefabs;
        char m_prefabSearch[256] = {0};
        int m_selectedPrefab = -1;
        std::string m_selectedCategory = "All";
    };

} // namespace SparkEditor
