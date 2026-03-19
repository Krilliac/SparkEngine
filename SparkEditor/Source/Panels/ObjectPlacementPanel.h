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

    /** @brief Editor panel for placing, scattering, and snapping objects in the scene. */
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

        /// Configuration for brush-mode object placement.
        struct PlacementBrush
        {
            float radius = 5.0f;           ///< Brush radius (meters).
            float density = 1.0f;          ///< Objects per square meter.
            float minScale = 0.8f;         ///< Minimum random scale.
            float maxScale = 1.2f;         ///< Maximum random scale.
            float minRotation = 0.0f;      ///< Minimum random Y rotation (degrees).
            float maxRotation = 360.0f;    ///< Maximum random Y rotation (degrees).
            bool randomizeRotation = true; ///< Apply random rotation to placed objects.
            bool randomizeScale = true;    ///< Apply random scale to placed objects.
            bool alignToSurface = true;    ///< Align placed objects to the surface normal.
        };

        /// Entry in the prefab library sidebar.
        struct PrefabEntry
        {
            std::string name;        ///< Prefab display name.
            std::string path;        ///< Asset path to the prefab file.
            std::string category;    ///< Category for filtering (e.g. "Props", "Foliage").
            bool isFavorite = false; ///< Whether this prefab is bookmarked.
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
