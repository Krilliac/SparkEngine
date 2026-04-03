/**
 * @file EELevelDesignSystem.h
 * @brief Level design tools: prefab placement, terrain sculpting, foliage painting
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides no-code level design tools including prefab placement with snap/grid,
 * terrain sculpting with multiple brush shapes, terrain layer painting,
 * foliage painting, spline path editing, and volume editing.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EngineEditor
{

    /// @brief A placed prefab instance in the level
    struct PrefabInstance
    {
        uint32_t instanceId = 0;
        uint32_t prefabId = 0;
        std::string prefabName;
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float rotX = 0.0f, rotY = 0.0f, rotZ = 0.0f;
        float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
        bool isLocked = false;
        bool isVisible = true;
        std::string layer = "Default";
    };

    /// @brief A registered prefab template
    struct PrefabTemplate
    {
        uint32_t prefabId = 0;
        std::string name;
        std::string category; ///< "Architecture", "Vegetation", "Props", etc.
        std::string meshPath;
        bool hasCollision = true;
        bool isStatic = true;
    };

    /// @brief Terrain brush configuration
    struct TerrainBrush
    {
        BrushShape shape = BrushShape::Circle;
        float radius = 10.0f;
        float strength = 0.5f;
        float falloff = 0.3f;
        TerrainLayer paintLayer = TerrainLayer::Grass;
    };

    /// @brief A spline control point for path editing
    struct SplinePoint
    {
        uint32_t pointId = 0;
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float tangentInX = 0.0f, tangentInY = 0.0f, tangentInZ = 0.0f;
        float tangentOutX = 0.0f, tangentOutY = 0.0f, tangentOutZ = 0.0f;
    };

    /// @brief A spline path in the level (roads, rivers, rails)
    struct SplinePath
    {
        uint32_t splineId = 0;
        std::string name;
        std::string category; ///< "Road", "River", "Rail", "Fence"
        std::vector<SplinePoint> points;
        bool isClosed = false;
        float width = 2.0f;
    };

    /**
     * @brief Level design toolkit for no-code environment creation
     *
     * Manages prefab placement with grid snapping, terrain sculpting,
     * layer painting, foliage placement, spline paths, and level layers.
     */
    class EELevelDesignSystem
    {
      public:
        EELevelDesignSystem() = default;
        ~EELevelDesignSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Prefab operations
        uint32_t PlacePrefab(uint32_t prefabId, float x, float y, float z);
        bool RemoveInstance(uint32_t instanceId);
        bool MoveInstance(uint32_t instanceId, float x, float y, float z);
        bool RotateInstance(uint32_t instanceId, float rx, float ry, float rz);
        bool ScaleInstance(uint32_t instanceId, float sx, float sy, float sz);

        // Terrain
        void SetBrush(BrushShape shape, float radius, float strength);
        void SculptTerrain(float x, float z, float amount);
        void PaintTerrain(float x, float z, TerrainLayer layer);

        // Splines
        uint32_t CreateSpline(const std::string& name, const std::string& category);
        bool AddSplinePoint(uint32_t splineId, float x, float y, float z);

        // Foliage
        void PaintFoliage(float x, float z, float radius, FoliageDensity density);

        // Queries
        size_t GetPrefabCount() const { return m_prefabTemplates.size(); }
        size_t GetInstanceCount() const { return m_instances.size(); }
        size_t GetSplineCount() const { return m_splines.size(); }
        std::string GetPrefabCatalogString() const;
        std::string GetInstanceListString() const;
        std::string GetSplineListString() const;
        std::string GetToolStatusString() const;

      private:
        void RegisterBuiltinPrefabs();

        Spark::IEngineContext* m_context{nullptr};
        LevelTool m_activeTool{LevelTool::Select};
        TerrainBrush m_brush;
        std::vector<PrefabTemplate> m_prefabTemplates;
        std::vector<PrefabInstance> m_instances;
        std::vector<SplinePath> m_splines;
        float m_gridSize{1.0f};
        bool m_snapToGrid{true};
        uint32_t m_nextInstanceId{1};
        uint32_t m_nextSplineId{1};
        uint32_t m_nextPointId{1};
        uint32_t m_foliageInstanceCount{0};
        bool m_initialized{false};
    };

} // namespace EngineEditor
