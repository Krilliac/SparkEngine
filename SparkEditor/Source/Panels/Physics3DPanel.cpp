/**
 * @file Physics3DPanel.cpp
 * @brief Implementation of the 3D Physics (Jolt) editor panel
 * @author Spark Engine Team
 * @date 2026
 */

#include "Physics3DPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace SparkEditor
{

    Physics3DPanel::Physics3DPanel() : EditorPanel("Physics 3D", "physics_3d_panel")
    {
        // Initialize collision layer matrix: all layers collide by default
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                m_layerMatrix[i][j] = true;
    }

    bool Physics3DPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        std::cout << "Initializing Physics 3D panel\n";
        return true;
    }

    void Physics3DPanel::Update(float /*deltaTime*/) {}

    void Physics3DPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderWorldSettings();
            ImGui::Spacing();

            RenderDebugVisualization();
            ImGui::Spacing();

            RenderPhysicsStats();
            ImGui::Spacing();

            RenderQuickAddPrimitives();
            ImGui::Spacing();

            RenderCollisionLayers();
            ImGui::Spacing();

            RenderMaterialPresets();
            ImGui::Spacing();

            RenderRaycastTool();

            EndPanel();
        }
    }

    void Physics3DPanel::Shutdown()
    {
        std::cout << "Shutting down Physics 3D panel\n";
    }

    // =========================================================================
    // World Settings
    // =========================================================================

    void Physics3DPanel::RenderWorldSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_GLOBE " World Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat3("Gravity", m_gravity, 0.1f, -100.0f, 100.0f);

            if (ImGui::Button("Earth"))
            {
                m_gravity[0] = 0.0f;
                m_gravity[1] = -9.81f;
                m_gravity[2] = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Moon"))
            {
                m_gravity[0] = 0.0f;
                m_gravity[1] = -1.62f;
                m_gravity[2] = 0.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Zero-G"))
            {
                m_gravity[0] = 0.0f;
                m_gravity[1] = 0.0f;
                m_gravity[2] = 0.0f;
            }

            ImGui::Separator();

            float fps = 1.0f / m_timeStep;
            if (ImGui::DragFloat("Physics FPS", &fps, 1.0f, 15.0f, 240.0f, "%.0f Hz"))
            {
                m_timeStep = 1.0f / std::max(15.0f, fps);
            }
            ImGui::DragInt("Max Substeps", &m_maxSubsteps, 1, 1, 32);
            ImGui::Checkbox("Deterministic Mode", &m_deterministicMode);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Ensures bit-identical results across runs.\n"
                                  "Required for lockstep networking and replay.\n"
                                  "~10-15%% performance cost.");
            }
        }
    }

    // =========================================================================
    // Debug Visualization
    // =========================================================================

    void Physics3DPanel::RenderDebugVisualization()
    {
        if (ImGui::CollapsingHeader(ICON_FA_EYE " Debug Visualization"))
        {
            ImGui::Checkbox("Wireframe Shapes", &m_showWireframes);
            ImGui::Checkbox("AABBs", &m_showAABBs);
            ImGui::Checkbox("Contact Points", &m_showContacts);
            ImGui::Checkbox("Constraints", &m_showConstraints);
            ImGui::Checkbox("Center of Mass", &m_showCenterOfMass);
            ImGui::Checkbox("Velocity Vectors", &m_showVelocityVectors);

            if (ImGui::Button("Enable All"))
            {
                m_showWireframes = m_showAABBs = m_showContacts = true;
                m_showConstraints = m_showCenterOfMass = m_showVelocityVectors = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable All"))
            {
                m_showWireframes = m_showAABBs = m_showContacts = false;
                m_showConstraints = m_showCenterOfMass = m_showVelocityVectors = false;
            }
        }
    }

    // =========================================================================
    // Stats
    // =========================================================================

    void Physics3DPanel::RenderPhysicsStats()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CHART_BAR " Statistics"))
        {
            ImGui::Text("Total Bodies:   %u", m_totalBodies);
            ImGui::Text("Active Bodies:  %u", m_activeBodies);
            ImGui::Text("Constraints:    %u", m_constraintCount);
            ImGui::Text("Sim Time:       %.2f ms", m_simTime);

            // Activity bar
            float ratio = m_totalBodies > 0 ? static_cast<float>(m_activeBodies) / m_totalBodies : 0.0f;
            ImGui::ProgressBar(ratio, ImVec2(-1, 0), "Active/Total");
        }
    }

    // =========================================================================
    // Quick-Add Physics Primitives
    // =========================================================================

    void Physics3DPanel::RenderQuickAddPrimitives()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CUBE " Quick Add Physics Object", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* shapes[] = {"Box", "Sphere", "Capsule", "Cylinder"};
            ImGui::Combo("Shape", &m_quickAddShape, shapes, 4);

            const char* bodyTypes[] = {"Static", "Kinematic", "Dynamic"};
            ImGui::Combo("Body Type", &m_quickAddBodyType, bodyTypes, 3);

            if (m_quickAddBodyType == 2) // Dynamic
            {
                ImGui::DragFloat("Mass (kg)", &m_quickAddMass, 0.1f, 0.01f, 100000.0f);
            }

            switch (m_quickAddShape)
            {
            case 0: // Box
                ImGui::DragFloat3("Dimensions", m_quickAddSize, 0.05f, 0.01f, 100.0f);
                break;
            case 1: // Sphere
                ImGui::DragFloat("Radius", &m_quickAddRadius, 0.05f, 0.01f, 50.0f);
                break;
            case 2: // Capsule
                ImGui::DragFloat("Radius", &m_quickAddRadius, 0.05f, 0.01f, 50.0f);
                ImGui::DragFloat("Height", &m_quickAddHeight, 0.05f, 0.01f, 100.0f);
                break;
            case 3: // Cylinder
                ImGui::DragFloat("Radius", &m_quickAddRadius, 0.05f, 0.01f, 50.0f);
                ImGui::DragFloat("Height", &m_quickAddHeight, 0.05f, 0.01f, 100.0f);
                break;
            }

            if (ImGui::Button(ICON_FA_PLUS " Add to Scene", ImVec2(-1, 30)))
            {
                // This will create an entity with Transform + RigidBody + Collider components
                // via the scene system when wired to the engine. For now, stores the intent.
                std::cout << "Add physics " << shapes[m_quickAddShape] << " (" << bodyTypes[m_quickAddBodyType]
                          << ", mass=" << m_quickAddMass << ")\n";
            }

            ImGui::Separator();
            ImGui::TextDisabled("Drag from here into Scene View to place objects.");
        }
    }

    // =========================================================================
    // Collision Layers
    // =========================================================================

    void Physics3DPanel::RenderCollisionLayers()
    {
        if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Collision Layers"))
        {
            // Layer names
            for (int i = 0; i < 8; ++i)
            {
                char label[32];
                snprintf(label, sizeof(label), "Layer %d", i);
                char buf[64];
                strncpy(buf, m_layerNames[i].c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                ImGui::PushItemWidth(120);
                if (ImGui::InputText(label, buf, sizeof(buf)))
                {
                    m_layerNames[i] = buf;
                }
                ImGui::PopItemWidth();
            }

            ImGui::Separator();
            ImGui::Text("Collision Matrix:");

            // Header row
            ImGui::Text("        ");
            for (int j = 0; j < 8; ++j)
            {
                ImGui::SameLine();
                ImGui::Text("%d", j);
                if (j < 7)
                    ImGui::SameLine(0, 12);
            }

            // Matrix checkboxes (lower triangle only — symmetric)
            for (int i = 0; i < 8; ++i)
            {
                ImGui::Text("Layer %d", i);
                for (int j = 0; j <= i; ++j)
                {
                    ImGui::SameLine();
                    char id[32];
                    snprintf(id, sizeof(id), "##col_%d_%d", i, j);
                    if (ImGui::Checkbox(id, &m_layerMatrix[i][j]))
                    {
                        m_layerMatrix[j][i] = m_layerMatrix[i][j]; // Symmetric
                    }
                }
            }
        }
    }

    // =========================================================================
    // Material Presets
    // =========================================================================

    void Physics3DPanel::RenderMaterialPresets()
    {
        if (ImGui::CollapsingHeader(ICON_FA_PALETTE " Material Presets"))
        {
            for (int i = 0; i < MAX_PRESETS; ++i)
            {
                auto& preset = m_presets[i];
                if (ImGui::TreeNode(preset.name.c_str()))
                {
                    ImGui::DragFloat("Friction", &preset.friction, 0.01f, 0.0f, 2.0f);
                    ImGui::DragFloat("Restitution", &preset.restitution, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Density", &preset.density, 1.0f, 0.1f, 20000.0f, "%.0f kg/m3");
                    ImGui::TreePop();
                }
            }
        }
    }

    // =========================================================================
    // Raycast Tool
    // =========================================================================

    void Physics3DPanel::RenderRaycastTool()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CROSSHAIRS " Raycast Test"))
        {
            ImGui::DragFloat3("Origin", m_rayOrigin, 0.1f);
            ImGui::DragFloat3("Direction", m_rayDirection, 0.01f, -1.0f, 1.0f);
            ImGui::DragFloat("Max Distance", &m_rayMaxDist, 1.0f, 0.1f, 10000.0f);

            if (ImGui::Button(ICON_FA_CROSSHAIRS " Cast Ray"))
            {
                // Would call PhysicsSystem::Raycast when wired to the engine
                std::cout << "Casting ray from (" << m_rayOrigin[0] << ", " << m_rayOrigin[1] << ", " << m_rayOrigin[2]
                          << ")\n";
                m_rayHit = false; // Reset until wired
            }

            if (m_rayHit)
            {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Hit at (%.2f, %.2f, %.2f) dist=%.2f",
                                   m_rayHitPoint[0], m_rayHitPoint[1], m_rayHitPoint[2], m_rayHitDistance);
            }
            else
            {
                ImGui::TextDisabled("No hit");
            }
        }
    }

} // namespace SparkEditor
