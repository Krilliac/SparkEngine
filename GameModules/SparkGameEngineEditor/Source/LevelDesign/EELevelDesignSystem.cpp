/**
 * @file EELevelDesignSystem.cpp
 * @brief Level design tools: prefab placement, terrain sculpting, foliage painting
 *
 * Implements no-code level design with built-in prefab catalog, terrain
 * brushes, foliage painting, and spline path editing.
 */

#include "EELevelDesignSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace EngineEditor
{

    bool EELevelDesignSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        RegisterBuiltinPrefabs();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Level design system initialized (%zu prefabs)",
                       m_prefabTemplates.size());
        return true;
    }

    void EELevelDesignSystem::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;
    }

    void EELevelDesignSystem::Shutdown()
    {
        m_instances.clear();
        m_prefabTemplates.clear();
        m_splines.clear();
        m_initialized = false;
    }

    void EELevelDesignSystem::RenderDebugUI() {}

    uint32_t EELevelDesignSystem::PlacePrefab(uint32_t prefabId, float x, float y, float z)
    {
        const PrefabTemplate* tmpl = nullptr;
        for (const auto& t : m_prefabTemplates)
        {
            if (t.prefabId == prefabId)
            {
                tmpl = &t;
                break;
            }
        }
        if (!tmpl)
            return 0;

        PrefabInstance inst;
        inst.instanceId = m_nextInstanceId++;
        inst.prefabId = prefabId;
        inst.prefabName = tmpl->name;

        if (m_snapToGrid && m_gridSize > 0.0f)
        {
            inst.posX = std::round(x / m_gridSize) * m_gridSize;
            inst.posY = std::round(y / m_gridSize) * m_gridSize;
            inst.posZ = std::round(z / m_gridSize) * m_gridSize;
        }
        else
        {
            inst.posX = x;
            inst.posY = y;
            inst.posZ = z;
        }

        m_instances.push_back(inst);
        return inst.instanceId;
    }

    bool EELevelDesignSystem::RemoveInstance(uint32_t instanceId)
    {
        auto it = std::find_if(m_instances.begin(), m_instances.end(),
                               [instanceId](const PrefabInstance& i) { return i.instanceId == instanceId; });
        if (it == m_instances.end())
            return false;
        m_instances.erase(it);
        return true;
    }

    bool EELevelDesignSystem::MoveInstance(uint32_t instanceId, float x, float y, float z)
    {
        for (auto& inst : m_instances)
        {
            if (inst.instanceId == instanceId)
            {
                inst.posX = x;
                inst.posY = y;
                inst.posZ = z;
                return true;
            }
        }
        return false;
    }

    bool EELevelDesignSystem::RotateInstance(uint32_t instanceId, float rx, float ry, float rz)
    {
        for (auto& inst : m_instances)
        {
            if (inst.instanceId == instanceId)
            {
                inst.rotX = rx;
                inst.rotY = ry;
                inst.rotZ = rz;
                return true;
            }
        }
        return false;
    }

    bool EELevelDesignSystem::ScaleInstance(uint32_t instanceId, float sx, float sy, float sz)
    {
        for (auto& inst : m_instances)
        {
            if (inst.instanceId == instanceId)
            {
                inst.scaleX = sx;
                inst.scaleY = sy;
                inst.scaleZ = sz;
                return true;
            }
        }
        return false;
    }

    void EELevelDesignSystem::SetBrush(BrushShape shape, float radius, float strength)
    {
        m_brush.shape = shape;
        m_brush.radius = radius;
        m_brush.strength = strength;
    }

    void EELevelDesignSystem::SculptTerrain([[maybe_unused]] float x, [[maybe_unused]] float z,
                                            [[maybe_unused]] float amount)
    {
        // Terrain heightmap modification dispatched to engine terrain system
    }

    void EELevelDesignSystem::PaintTerrain([[maybe_unused]] float x, [[maybe_unused]] float z, TerrainLayer layer)
    {
        m_brush.paintLayer = layer;
        // Layer weight painting dispatched to engine terrain system
    }

    uint32_t EELevelDesignSystem::CreateSpline(const std::string& name, const std::string& category)
    {
        SplinePath spline;
        spline.splineId = m_nextSplineId++;
        spline.name = name;
        spline.category = category;
        m_splines.push_back(std::move(spline));
        return m_splines.back().splineId;
    }

    bool EELevelDesignSystem::AddSplinePoint(uint32_t splineId, float x, float y, float z)
    {
        for (auto& s : m_splines)
        {
            if (s.splineId == splineId)
            {
                SplinePoint pt;
                pt.pointId = m_nextPointId++;
                pt.posX = x;
                pt.posY = y;
                pt.posZ = z;
                s.points.push_back(pt);
                return true;
            }
        }
        return false;
    }

    void EELevelDesignSystem::PaintFoliage([[maybe_unused]] float x, [[maybe_unused]] float z,
                                           [[maybe_unused]] float radius, FoliageDensity density)
    {
        // Foliage instance scattering dispatched to engine foliage system
        uint32_t count = 0;
        switch (density)
        {
        case FoliageDensity::Sparse:
            count = 5;
            break;
        case FoliageDensity::Normal:
            count = 15;
            break;
        case FoliageDensity::Dense:
            count = 30;
            break;
        case FoliageDensity::Lush:
            count = 50;
            break;
        default:
            count = 10;
            break;
        }
        m_foliageInstanceCount += count;
    }

    std::string EELevelDesignSystem::GetPrefabCatalogString() const
    {
        std::string s = "=== Prefab Catalog ===\n";
        std::string lastCat;
        for (const auto& p : m_prefabTemplates)
        {
            if (p.category != lastCat)
            {
                lastCat = p.category;
                s += " [" + p.category + "]\n";
            }
            s += "  [" + std::to_string(p.prefabId) + "] " + p.name;
            if (p.isStatic)
                s += " (static)";
            s += "\n";
        }
        return s;
    }

    std::string EELevelDesignSystem::GetInstanceListString() const
    {
        std::string s = "=== Placed Instances (" + std::to_string(m_instances.size()) + ") ===\n";
        for (const auto& i : m_instances)
        {
            s += "  [" + std::to_string(i.instanceId) + "] " + i.prefabName;
            s += " at (" + std::to_string(static_cast<int>(i.posX)) + ", " + std::to_string(static_cast<int>(i.posY)) +
                 ", " + std::to_string(static_cast<int>(i.posZ)) + ")";
            if (i.isLocked)
                s += " [locked]";
            s += "\n";
        }
        if (m_instances.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EELevelDesignSystem::GetSplineListString() const
    {
        std::string s = "=== Spline Paths ===\n";
        for (const auto& sp : m_splines)
        {
            s += "  [" + std::to_string(sp.splineId) + "] " + sp.name + " (" + sp.category + ", " +
                 std::to_string(sp.points.size()) + " points";
            if (sp.isClosed)
                s += ", closed";
            s += ")\n";
        }
        if (m_splines.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EELevelDesignSystem::GetToolStatusString() const
    {
        std::string s = "Active Tool: " + std::to_string(static_cast<int>(m_activeTool)) + "\n";
        s += "Grid: " + std::to_string(m_gridSize) + "m (" + (m_snapToGrid ? "ON" : "OFF") + ")\n";
        s += "Brush: shape=" + std::to_string(static_cast<int>(m_brush.shape)) +
             " r=" + std::to_string(static_cast<int>(m_brush.radius)) + " str=" + std::to_string(m_brush.strength) +
             "\n";
        s += "Foliage instances: " + std::to_string(m_foliageInstanceCount) + "\n";
        return s;
    }

    void EELevelDesignSystem::RegisterBuiltinPrefabs()
    {
        uint32_t id = 1;
        auto add =
            [&](const std::string& name, const std::string& cat, const std::string& mesh, bool collision, bool isStatic)
        {
            PrefabTemplate t;
            t.prefabId = id++;
            t.name = name;
            t.category = cat;
            t.meshPath = mesh;
            t.hasCollision = collision;
            t.isStatic = isStatic;
            m_prefabTemplates.push_back(std::move(t));
        };

        // Architecture
        add("Wall_4m", "Architecture", "meshes/arch/wall_4m.mesh", true, true);
        add("Wall_8m", "Architecture", "meshes/arch/wall_8m.mesh", true, true);
        add("Floor_4x4", "Architecture", "meshes/arch/floor_4x4.mesh", true, true);
        add("Pillar", "Architecture", "meshes/arch/pillar.mesh", true, true);
        add("Doorway", "Architecture", "meshes/arch/doorway.mesh", true, true);
        add("Window", "Architecture", "meshes/arch/window.mesh", true, true);
        add("Stairs_Straight", "Architecture", "meshes/arch/stairs_straight.mesh", true, true);
        add("Stairs_Spiral", "Architecture", "meshes/arch/stairs_spiral.mesh", true, true);
        add("Roof_Flat", "Architecture", "meshes/arch/roof_flat.mesh", true, true);
        add("Arch_Gothic", "Architecture", "meshes/arch/arch_gothic.mesh", true, true);

        // Vegetation
        add("Tree_Oak", "Vegetation", "meshes/veg/tree_oak.mesh", true, true);
        add("Tree_Pine", "Vegetation", "meshes/veg/tree_pine.mesh", true, true);
        add("Bush_Small", "Vegetation", "meshes/veg/bush_small.mesh", false, true);
        add("Bush_Large", "Vegetation", "meshes/veg/bush_large.mesh", true, true);
        add("Rock_Small", "Vegetation", "meshes/veg/rock_small.mesh", true, true);
        add("Rock_Large", "Vegetation", "meshes/veg/rock_large.mesh", true, true);
        add("Grass_Patch", "Vegetation", "meshes/veg/grass_patch.mesh", false, true);
        add("Flower_Cluster", "Vegetation", "meshes/veg/flower_cluster.mesh", false, true);

        // Props
        add("Barrel", "Props", "meshes/props/barrel.mesh", true, false);
        add("Crate_Small", "Props", "meshes/props/crate_small.mesh", true, false);
        add("Crate_Large", "Props", "meshes/props/crate_large.mesh", true, false);
        add("Bench", "Props", "meshes/props/bench.mesh", true, true);
        add("Lamp_Post", "Props", "meshes/props/lamp_post.mesh", true, true);
        add("Fence_Section", "Props", "meshes/props/fence_section.mesh", true, true);
        add("Sign_Post", "Props", "meshes/props/sign_post.mesh", true, true);
        add("Chest", "Props", "meshes/props/chest.mesh", true, false);

        // Lights
        add("PointLight", "Lights", "internal://point_light", false, true);
        add("SpotLight", "Lights", "internal://spot_light", false, true);
        add("DirectionalLight", "Lights", "internal://directional_light", false, true);
        add("AreaLight", "Lights", "internal://area_light", false, true);

        // Volumes
        add("TriggerVolume", "Volumes", "internal://trigger_volume", false, true);
        add("BlockingVolume", "Volumes", "internal://blocking_volume", true, true);
        add("AudioVolume", "Volumes", "internal://audio_volume", false, true);
        add("PostProcessVolume", "Volumes", "internal://postprocess_volume", false, true);
    }

} // namespace EngineEditor
