/**
 * @file ObjectPlacementPanel.cpp
 * @brief Object placement and snapping tools panel implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "ObjectPlacementPanel.h"
#include <iostream>

#include <imgui.h>

#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"

namespace SparkEditor
{

    ObjectPlacementPanel::ObjectPlacementPanel() : EditorPanel("Object Placement", "object_placement_panel") {}

    ObjectPlacementPanel::~ObjectPlacementPanel() {}

    bool ObjectPlacementPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ObjectPlacementPanel initialized");

        // Populate built-in prefab library
        m_prefabs.push_back({"Cube", "Primitives/Cube", "Primitives", false});
        m_prefabs.push_back({"Sphere", "Primitives/Sphere", "Primitives", false});
        m_prefabs.push_back({"Cylinder", "Primitives/Cylinder", "Primitives", false});
        m_prefabs.push_back({"Plane", "Primitives/Plane", "Primitives", false});
        m_prefabs.push_back({"Cone", "Primitives/Cone", "Primitives", false});
        m_prefabs.push_back({"Capsule", "Primitives/Capsule", "Primitives", false});

        m_prefabs.push_back({"Point Light", "Lights/PointLight", "Lights", false});
        m_prefabs.push_back({"Spot Light", "Lights/SpotLight", "Lights", false});
        m_prefabs.push_back({"Directional Light", "Lights/DirectionalLight", "Lights", false});

        m_prefabs.push_back({"Camera", "Cameras/Camera", "Cameras", false});

        m_prefabs.push_back({"Player Spawn", "FPS/PlayerSpawn", "FPS", false});
        m_prefabs.push_back({"Weapon Pickup", "FPS/WeaponPickup", "FPS", false});
        m_prefabs.push_back({"Health Pack", "FPS/HealthPack", "FPS", false});
        m_prefabs.push_back({"Ammo Crate", "FPS/AmmoCrate", "FPS", false});
        m_prefabs.push_back({"Cover Wall", "FPS/CoverWall", "FPS", false});
        m_prefabs.push_back({"Explosive Barrel", "FPS/ExplosiveBarrel", "FPS", false});

        m_prefabs.push_back({"Tree", "Environment/Tree", "Environment", false});
        m_prefabs.push_back({"Rock", "Environment/Rock", "Environment", false});
        m_prefabs.push_back({"Crate", "Environment/Crate", "Environment", false});
        m_prefabs.push_back({"Barrel", "Environment/Barrel", "Environment", false});

        m_prefabs.push_back({"Trigger Volume", "Volumes/TriggerVolume", "Volumes", false});
        m_prefabs.push_back({"Post-Process Volume", "Volumes/PostProcessVolume", "Volumes", false});
        m_prefabs.push_back({"Fog Volume", "Volumes/FogVolume", "Volumes", false});
        m_prefabs.push_back({"Audio Reverb Zone", "Volumes/AudioReverbZone", "Volumes", false});

        m_prefabs.push_back({"Reflection Probe", "Probes/ReflectionProbe", "Probes", false});
        m_prefabs.push_back({"Light Probe", "Probes/LightProbe", "Probes", false});

        m_prefabs.push_back({"Water Plane", "Environment/WaterPlane", "Environment", false});
        m_prefabs.push_back({"Spawn Point", "Gameplay/SpawnPoint", "Gameplay", false});
        m_prefabs.push_back({"NavMesh Obstacle", "AI/NavMeshObstacle", "AI", false});

        m_prefabs.push_back({"Wind Zone", "Volumes/WindZone", "Volumes", false});
        m_prefabs.push_back({"Cinematic Trigger", "Volumes/CinematicTrigger", "Volumes", false});
        m_prefabs.push_back({"Area Boundary", "Volumes/AreaBoundary", "Volumes", false});

        m_prefabs.push_back({"Occluder", "Environment/Occluder", "Environment", false});
        m_prefabs.push_back({"Billboard", "Environment/Billboard", "Environment", false});

        m_prefabs.push_back({"Physics Joint", "Physics/Joint", "Physics", false});
        m_prefabs.push_back({"Destructible", "Gameplay/Destructible", "Gameplay", false});
        m_prefabs.push_back({"Dialogue Trigger", "Gameplay/DialogueTrigger", "Gameplay", false});

        m_prefabs.push_back({"Cover Point", "AI/CoverPoint", "AI", false});
        m_prefabs.push_back({"Tactical Point", "AI/TacticalPoint", "AI", false});
        m_prefabs.push_back({"Nav Region", "AI/NavRegion", "AI", false});
        m_prefabs.push_back({"Nav Link", "AI/NavLink", "AI", false});

        m_prefabs.push_back({"Character Controller", "Gameplay/CharacterController", "Gameplay", false});
        m_prefabs.push_back({"Vehicle", "Gameplay/Vehicle", "Gameplay", false});

        m_prefabs.push_back({"Ragdoll", "Physics/Ragdoll", "Physics", false});
        m_prefabs.push_back({"Soft Body", "Physics/SoftBody", "Physics", false});
        m_prefabs.push_back({"Constant Force", "Physics/ConstantForce", "Physics", false});
        m_prefabs.push_back({"Force Region", "Volumes/ForceRegion", "Volumes", false});
        m_prefabs.push_back({"Buoyancy Volume", "Volumes/BuoyancyVolume", "Volumes", false});
        m_prefabs.push_back({"Spring Arm", "Physics/SpringArm", "Physics", false});

        m_prefabs.push_back({"Skybox", "Rendering/Skybox", "Rendering", false});
        m_prefabs.push_back({"Line Renderer", "Rendering/LineRenderer", "Rendering", false});
        m_prefabs.push_back({"Trail Renderer", "Rendering/TrailRenderer", "Rendering", false});
        m_prefabs.push_back({"Text 3D", "Rendering/Text3D", "Rendering", false});
        m_prefabs.push_back({"Foliage Volume", "Environment/FoliageVolume", "Environment", false});

        return true;
    }

    void ObjectPlacementPanel::Update(float deltaTime) {}

    void ObjectPlacementPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderPlacementModeSelector();
            ImGui::Separator();

            if (ImGui::CollapsingHeader(ICON_FA_TH " Snap Settings", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderSnapSettings();
            }

            if (m_placementMode == PlacementMode::Brush || m_placementMode == PlacementMode::Scatter)
            {
                if (ImGui::CollapsingHeader(ICON_FA_PAINT_BRUSH " Brush Settings", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    RenderBrushSettings();
                }
            }

            if (ImGui::CollapsingHeader(ICON_FA_LOCK " Transform Constraints"))
            {
                RenderTransformConstraints();
            }

            ImGui::Separator();

            if (ImGui::CollapsingHeader(ICON_FA_CUBE " Prefab Library", ImGuiTreeNodeFlags_DefaultOpen))
            {
                RenderPrefabLibrary();
            }

            ImGui::Separator();
            RenderQuickPlace();
        }
        EndPanel();
    }

    void ObjectPlacementPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ObjectPlacementPanel shutting down");
    }

    bool ObjectPlacementPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        return false;
    }

    void ObjectPlacementPanel::RenderPlacementModeSelector()
    {
        ImGui::Text(ICON_FA_MOUSE_POINTER " Placement Mode");
        ImGui::Spacing();

        float btnWidth = (ImGui::GetContentRegionAvail().x - 16.0f) / 5.0f;
        ImVec2 btnSize(btnWidth, 28.0f);

        auto ModeButton = [&](const char* label, PlacementMode mode)
        {
            bool active = (m_placementMode == mode);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.55f, 0.9f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.95f, 1.0f));
            }
            if (ImGui::Button(label, btnSize))
                m_placementMode = mode;
            if (active)
                ImGui::PopStyleColor(2);
            ImGui::SameLine();
        };

        ModeButton("Single", PlacementMode::Single);
        ModeButton("Brush", PlacementMode::Brush);
        ModeButton("Line", PlacementMode::Line);
        ModeButton("Grid", PlacementMode::Grid);
        ModeButton("Scatter", PlacementMode::Scatter);
        ImGui::NewLine();

        // Align mode
        ImGui::Text("Align To:");
        ImGui::SameLine();
        const char* alignModes[] = {"None", "Surface", "Grid", "Vertex"};
        int alignIdx = static_cast<int>(m_alignMode);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##AlignMode", &alignIdx, alignModes, 4))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "ObjectPlacementPanel: align mode changed to '%s'",
                            alignModes[alignIdx]);
            m_alignMode = static_cast<AlignMode>(alignIdx);
        }
    }

    void ObjectPlacementPanel::RenderSnapSettings()
    {
        ImGui::Indent(8.0f);

        ImGui::Checkbox("Snap to Grid", &m_snapToGrid);
        if (m_snapToGrid)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("##GridSize", &m_snapGridSize, 0.1f, 0.1f, 100.0f, "%.1f m");
        }

        ImGui::Checkbox("Snap Rotation", &m_snapRotation);
        if (m_snapRotation)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("##RotSnap", &m_snapRotationAngle, 1.0f, 1.0f, 90.0f, "%.0f\xc2\xb0");
        }

        ImGui::Checkbox("Snap Scale", &m_snapScale);
        if (m_snapScale)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("##ScaleSnap", &m_snapScaleIncrement, 0.05f, 0.05f, 5.0f, "%.2f");
        }

        ImGui::Checkbox("Snap to Surface", &m_snapToSurface);
        ImGui::Checkbox("Snap to Vertex", &m_snapToVertex);

        ImGui::Unindent(8.0f);
    }

    void ObjectPlacementPanel::RenderBrushSettings()
    {
        ImGui::Indent(8.0f);

        ImGui::DragFloat("Brush Radius", &m_brush.radius, 0.5f, 0.5f, 100.0f, "%.1f m");
        ImGui::DragFloat("Density", &m_brush.density, 0.1f, 0.1f, 10.0f, "%.1f");

        ImGui::Checkbox("Randomize Scale", &m_brush.randomizeScale);
        if (m_brush.randomizeScale)
        {
            ImGui::DragFloatRange2("Scale Range", &m_brush.minScale, &m_brush.maxScale, 0.05f, 0.1f, 5.0f, "%.2f",
                                   "%.2f");
        }

        ImGui::Checkbox("Randomize Rotation", &m_brush.randomizeRotation);
        if (m_brush.randomizeRotation)
        {
            ImGui::DragFloatRange2("Rotation Range", &m_brush.minRotation, &m_brush.maxRotation, 1.0f, 0.0f, 360.0f,
                                   "%.0f\xc2\xb0", "%.0f\xc2\xb0");
        }

        ImGui::Checkbox("Align to Surface Normal", &m_brush.alignToSurface);

        ImGui::Unindent(8.0f);
    }

    void ObjectPlacementPanel::RenderTransformConstraints()
    {
        ImGui::Indent(8.0f);

        ImGui::Text("Lock Axes:");
        ImGui::SameLine();

        ImVec4 lockColor(0.9f, 0.2f, 0.2f, 1.0f);
        auto LockButton = [&](const char* label, bool& locked)
        {
            if (locked)
                ImGui::PushStyleColor(ImGuiCol_Button, lockColor);
            if (ImGui::Button(label, ImVec2(30, 24)))
                locked = !locked;
            if (locked)
                ImGui::PopStyleColor();
            ImGui::SameLine();
        };

        LockButton("X", m_lockX);
        LockButton("Y", m_lockY);
        LockButton("Z", m_lockZ);
        ImGui::NewLine();

        ImGui::Checkbox("Uniform Scale", &m_uniformScale);

        ImGui::Unindent(8.0f);
    }

    void ObjectPlacementPanel::RenderPrefabLibrary()
    {
        ImGui::Indent(8.0f);

        // Search bar
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##PrefabSearch", ICON_FA_SEARCH " Search prefabs...", m_prefabSearch,
                                 sizeof(m_prefabSearch));

        // Category filter
        std::vector<std::string> categories = {"All"};
        for (const auto& p : m_prefabs)
        {
            bool found = false;
            for (const auto& c : categories)
            {
                if (c == p.category)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                categories.push_back(p.category);
        }

        if (ImGui::BeginCombo("Category", m_selectedCategory.c_str()))
        {
            for (const auto& cat : categories)
            {
                bool isSelected = (m_selectedCategory == cat);
                if (ImGui::Selectable(cat.c_str(), isSelected))
                    m_selectedCategory = cat;
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Prefab list
        ImGui::BeginChild("##PrefabList", ImVec2(0, 200), true);
        for (int i = 0; i < static_cast<int>(m_prefabs.size()); ++i)
        {
            const auto& prefab = m_prefabs[i];

            // Filter by category
            if (m_selectedCategory != "All" && prefab.category != m_selectedCategory)
                continue;

            // Filter by search
            if (m_prefabSearch[0] != '\0')
            {
                std::string lower = prefab.name;
                std::string filter = m_prefabSearch;
                for (auto& c : lower)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (auto& c : filter)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (!lower.contains(filter))
                    continue;
            }

            bool isSelected = (m_selectedPrefab == i);
            const char* icon = ICON_FA_CUBE;
            if (prefab.category == "Lights")
                icon = ICON_FA_LIGHTBULB;
            else if (prefab.category == "Cameras")
                icon = ICON_FA_CAMERA;
            else if (prefab.category == "FPS")
                icon = ICON_FA_CROSSHAIRS;
            else if (prefab.category == "Environment")
                icon = ICON_FA_TREE;
            else if (prefab.category == "Volumes")
                icon = ICON_FA_VECTOR_SQUARE;
            else if (prefab.category == "Probes")
                icon = ICON_FA_DOT_CIRCLE;
            else if (prefab.category == "Gameplay")
                icon = ICON_FA_FLAG;
            else if (prefab.category == "AI")
                icon = ICON_FA_BRAIN;
            else if (prefab.category == "Physics")
                icon = ICON_FA_VECTOR_SQUARE;
            else if (prefab.category == "Rendering")
                icon = ICON_FA_IMAGE;

            std::string label = std::string(icon) + "  " + prefab.name;
            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                m_selectedPrefab = i;
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Category: %s\nPath: %s", prefab.category.c_str(), prefab.path.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Unindent(8.0f);
    }

    void ObjectPlacementPanel::RenderQuickPlace()
    {
        ImGui::Text(ICON_FA_BOLT " Quick Place");
        ImGui::Spacing();

        float btnWidth = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        ImVec2 btnSize(btnWidth, 28.0f);

        if (ImGui::Button(ICON_FA_CUBE " Cube", btnSize))
        {
            // Trigger placement via event
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CIRCLE " Sphere", btnSize))
        {
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_VECTOR_SQUARE " Plane", btnSize))
        {
        }

        if (ImGui::Button(ICON_FA_LIGHTBULB " Light", btnSize))
        {
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CAMERA " Camera", btnSize))
        {
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FLAG " Spawn", btnSize))
        {
        }
    }

} // namespace SparkEditor
