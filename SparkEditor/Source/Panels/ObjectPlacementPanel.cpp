/**
 * @file ObjectPlacementPanel.cpp
 * @brief Object placement and snapping tools panel implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "ObjectPlacementPanel.h"

#include <imgui.h>

#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"

#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

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

            if (Spark::Ui::SectionHeader("Snap Settings", ICON_FA_TH))
            {
                RenderSnapSettings();
            }

            if (m_placementMode == PlacementMode::Brush || m_placementMode == PlacementMode::Scatter)
            {
                if (Spark::Ui::SectionHeader("Brush Settings", ICON_FA_PAINT_BRUSH))
                {
                    RenderBrushSettings();
                }
            }

            if (Spark::Ui::SectionHeader("Transform Constraints", ICON_FA_LOCK, false))
            {
                RenderTransformConstraints();
            }

            ImGui::Separator();

            if (Spark::Ui::SectionHeader("Prefab Library", ICON_FA_CUBE))
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
        Spark::Ui::SubHeader(ICON_FA_MOUSE_POINTER " Placement Mode");
        Spark::Ui::Banner(Spark::Ui::Status::Info,
                          "Preview - the scene viewport does not read the mode, align, snap or brush settings below "
                          "yet; Quick Place and Place Selected do work.");
        ImGui::Spacing();

        // Order matches PlacementMode, so the enum value is the segment index.
        const char* modeLabels[] = {"Single", "Brush", "Line", "Grid", "Scatter"};
        int modeIndex = static_cast<int>(m_placementMode);
        if (Spark::Ui::SegmentedControl("PlacementMode", modeLabels, 5, &modeIndex))
        {
            m_placementMode = static_cast<PlacementMode>(modeIndex);
        }

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
        ImGui::Indent(Spark::Ui::Px(8.0f));

        ImGui::Checkbox("Snap to Grid", &m_snapToGrid);
        if (m_snapToGrid)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Spark::Ui::Px(80));
            ImGui::DragFloat("##GridSize", &m_snapGridSize, 0.1f, 0.1f, 100.0f, "%.1f m");
        }

        ImGui::Checkbox("Snap Rotation", &m_snapRotation);
        if (m_snapRotation)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Spark::Ui::Px(80));
            ImGui::DragFloat("##RotSnap", &m_snapRotationAngle, 1.0f, 1.0f, 90.0f, "%.0f\xc2\xb0");
        }

        ImGui::Checkbox("Snap Scale", &m_snapScale);
        if (m_snapScale)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Spark::Ui::Px(80));
            ImGui::DragFloat("##ScaleSnap", &m_snapScaleIncrement, 0.05f, 0.05f, 5.0f, "%.2f");
        }

        ImGui::Checkbox("Snap to Surface", &m_snapToSurface);
        ImGui::Checkbox("Snap to Vertex", &m_snapToVertex);

        ImGui::Unindent(Spark::Ui::Px(8.0f));
    }

    void ObjectPlacementPanel::RenderBrushSettings()
    {
        ImGui::Indent(Spark::Ui::Px(8.0f));

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

        ImGui::Unindent(Spark::Ui::Px(8.0f));
    }

    void ObjectPlacementPanel::RenderTransformConstraints()
    {
        ImGui::Indent(Spark::Ui::Px(8.0f));

        ImGui::Text("Lock Axes:");
        ImGui::SameLine();

        const ImVec2 lockSize = Spark::Ui::Px2(30, 24);
        auto LockButton = [&](const char* label, bool& locked)
        {
            bool clicked = false;
            if (locked)
            {
                // A locked axis reads as "on": accent fill, and the tooltip names the state.
                Spark::Ui::ScopedPrimaryButtonColors lockedColours;
                clicked = ImGui::Button(label, lockSize);
            }
            else
            {
                clicked = ImGui::Button(label, lockSize);
            }
            ImGui::SetItemTooltip("%s axis %s", label, locked ? "locked" : "unlocked");
            if (clicked)
                locked = !locked;
            ImGui::SameLine();
        };

        LockButton("X", m_lockX);
        LockButton("Y", m_lockY);
        LockButton("Z", m_lockZ);
        ImGui::NewLine();

        ImGui::Checkbox("Uniform Scale", &m_uniformScale);

        ImGui::Unindent(Spark::Ui::Px(8.0f));
    }

    void ObjectPlacementPanel::RenderPrefabLibrary()
    {
        ImGui::Indent(Spark::Ui::Px(8.0f));

        // Search bar
        Spark::Ui::SearchField("PrefabSearch", m_prefabSearch, sizeof(m_prefabSearch), "Search prefabs...");

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
        ImGui::BeginChild("##PrefabList", Spark::Ui::Px2(0, 200), true);
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

        const bool hasSelection = m_selectedPrefab >= 0 && m_selectedPrefab < static_cast<int>(m_prefabs.size());
        ImGui::BeginDisabled(!hasSelection || !IsPlacementConnected());
        if (Spark::Ui::PrimaryButton(ICON_FA_PLUS " Place Selected") && hasSelection)
        {
            PlaceTemplate(m_prefabs[static_cast<size_t>(m_selectedPrefab)].name);
        }
        ImGui::EndDisabled();

        ImGui::Unindent(Spark::Ui::Px(8.0f));
    }

    bool ObjectPlacementPanel::PlaceTemplate(const std::string& templateName)
    {
        if (!m_entityCreator)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "ObjectPlacementPanel: not connected to the editor document; '%s' was not placed",
                           templateName.c_str());
            return false;
        }

        if (!m_entityCreator(templateName))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "ObjectPlacementPanel: '%s' is not a supported entity template",
                           templateName.c_str());
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ObjectPlacementPanel: placed '%s'", templateName.c_str());
        return true;
    }

    void ObjectPlacementPanel::RenderQuickPlace()
    {
        Spark::Ui::SubHeader(ICON_FA_BOLT " Quick Place");
        if (!IsPlacementConnected())
        {
            Spark::Ui::UnavailableFeature("Quick Place",
                                          "Preview - not connected: this panel has no editor document to place into.");
        }
        ImGui::Spacing();

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float btnWidth = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
        const ImVec2 btnSize(btnWidth, Spark::Ui::Px(28.0f));

        ImGui::BeginDisabled(!IsPlacementConnected());

        if (ImGui::Button(ICON_FA_CUBE " Cube", btnSize))
            PlaceTemplate("Cube");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CIRCLE " Sphere", btnSize))
            PlaceTemplate("Sphere");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_VECTOR_SQUARE " Plane", btnSize))
            PlaceTemplate("Plane");

        if (ImGui::Button(ICON_FA_LIGHTBULB " Light", btnSize))
            PlaceTemplate("Point Light");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CAMERA " Camera", btnSize))
            PlaceTemplate("Camera");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FLAG " Spawn", btnSize))
            PlaceTemplate("Spawn Point");

        ImGui::EndDisabled();
    }

} // namespace SparkEditor
