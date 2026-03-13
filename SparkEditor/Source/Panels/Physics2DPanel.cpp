/**
 * @file Physics2DPanel.cpp
 * @brief Implementation of the 2D Physics panel
 * @author Spark Engine Team
 * @date 2026
 */

#include "Physics2DPanel.h"
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

    Physics2DPanel::Physics2DPanel() : EditorPanel("Physics 2D", "physics_2d_panel")
    {
        // Initialize layer matrix: all layers collide by default
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 16; ++j)
                m_layerMatrix[i][j] = true;
    }

    bool Physics2DPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        std::cout << "Initializing Physics 2D panel\n";
        return true;
    }

    void Physics2DPanel::Update(float /*deltaTime*/) {}

    void Physics2DPanel::Render()
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

            RenderCollisionLayers();
            ImGui::Spacing();

            RenderBodyInspector();
            ImGui::Spacing();

            RenderRaycastTool();

            EndPanel();
        }
    }

    void Physics2DPanel::Shutdown()
    {
        std::cout << "Shutting down Physics 2D panel\n";
    }

    void Physics2DPanel::RenderWorldSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_GLOBE " World Settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat2("Gravity", m_gravity, 0.1f, -100.0f, 100.0f);

            if (ImGui::Button("Earth (0, -9.81)"))
            {
                m_gravity[0] = 0.0f;
                m_gravity[1] = -9.81f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Moon (0, -1.62)"))
            {
                m_gravity[0] = 0.0f;
                m_gravity[1] = -1.62f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Zero"))
            {
                m_gravity[0] = 0.0f;
                m_gravity[1] = 0.0f;
            }

            static int velocityIters = 8;
            static int positionIters = 3;
            ImGui::DragInt("Velocity Iterations", &velocityIters, 0.1f, 1, 20);
            ImGui::DragInt("Position Iterations", &positionIters, 0.1f, 1, 20);

            static float fixedTimestep = 1.0f / 60.0f;
            ImGui::DragFloat("Fixed Timestep", &fixedTimestep, 0.001f, 0.001f, 0.1f, "%.3f s");
            ImGui::Text("%.0f Hz", 1.0f / fixedTimestep);
        }
    }

    void Physics2DPanel::RenderDebugVisualization()
    {
        if (ImGui::CollapsingHeader(ICON_FA_EYE " Debug Visualization", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Show AABBs", &m_showAABBs);
            ImGui::Checkbox("Show Velocities", &m_showVelocities);
            ImGui::Checkbox("Show Contacts", &m_showContacts);
            ImGui::Checkbox("Show Collision Normals", &m_showCollisionNormals);
            ImGui::Checkbox("Show Spatial Hash Grid", &m_showSpatialHash);

            ImGui::Spacing();

            static float aabbColor[4] = {0.0f, 1.0f, 0.0f, 0.5f};
            static float velocityColor[4] = {1.0f, 1.0f, 0.0f, 0.8f};
            static float contactColor[4] = {1.0f, 0.0f, 0.0f, 1.0f};

            if (ImGui::TreeNode("Debug Colors"))
            {
                ImGui::ColorEdit4("AABB Color", aabbColor, ImGuiColorEditFlags_AlphaBar);
                ImGui::ColorEdit4("Velocity Color", velocityColor, ImGuiColorEditFlags_AlphaBar);
                ImGui::ColorEdit4("Contact Color", contactColor, ImGuiColorEditFlags_AlphaBar);
                ImGui::TreePop();
            }
        }
    }

    void Physics2DPanel::RenderPhysicsStats()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CHART_BAR " Physics Stats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Bodies:     %d", m_bodyCount);
            ImGui::Text("  Static:   %d", 0);
            ImGui::Text("  Kinematic: %d", 0);
            ImGui::Text("  Dynamic:  %d", 0);
            ImGui::Text("Contacts:   %d", m_contactCount);
            ImGui::Text("Step Time:  %.2f ms", m_stepTime);

            // Performance bar
            float maxStepTime = 16.67f; // 60 FPS budget
            float ratio = m_stepTime / maxStepTime;
            ImVec4 barColor = (ratio < 0.5f)   ? ImVec4(0, 1, 0, 1)
                              : (ratio < 0.8f) ? ImVec4(1, 1, 0, 1)
                                               : ImVec4(1, 0, 0, 1);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
            ImGui::ProgressBar(ratio, ImVec2(-1, 0), "Physics Budget");
            ImGui::PopStyleColor();
        }
    }

    void Physics2DPanel::RenderCollisionLayers()
    {
        if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Collision Layer Matrix"))
        {
            ImGui::Text("Configure which layers collide with each other:");
            ImGui::Spacing();

            // Layer name editor
            if (ImGui::TreeNode("Layer Names"))
            {
                for (int i = 0; i < 16; ++i)
                {
                    char label[32];
                    std::snprintf(label, sizeof(label), "Layer %d", i);
                    char buf[64];
                    std::strncpy(buf, m_layerNames[i].c_str(), sizeof(buf) - 1);
                    buf[sizeof(buf) - 1] = '\0';
                    if (ImGui::InputText(label, buf, sizeof(buf)))
                        m_layerNames[i] = buf;
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();

            // Draw collision matrix (show 8x8 for usability)
            int visibleLayers = 8;
            float cellSize = 24.0f;

            // Column headers
            ImGui::Dummy(ImVec2(80, 0));
            for (int j = 0; j < visibleLayers; ++j)
            {
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cellSize * 0.2f);
                ImGui::Text("%d", j);
            }

            for (int i = 0; i < visibleLayers; ++i)
            {
                ImGui::Text("%-8s", m_layerNames[i].substr(0, 8).c_str());
                for (int j = 0; j <= i; ++j)
                {
                    ImGui::SameLine();
                    char cbLabel[32];
                    std::snprintf(cbLabel, sizeof(cbLabel), "##L%d_%d", i, j);
                    if (ImGui::Checkbox(cbLabel, &m_layerMatrix[i][j]))
                    {
                        m_layerMatrix[j][i] = m_layerMatrix[i][j]; // Keep symmetric
                    }
                }
            }
        }
    }

    void Physics2DPanel::RenderBodyInspector()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CUBE " Body Inspector"))
        {
            if (!m_scene)
            {
                ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, 1.0f),
                                   ICON_FA_INFO_CIRCLE " No scene loaded. Load a scene to inspect physics bodies.");
                return;
            }

            ImGui::Text("Select a physics entity to inspect its 2D body properties.");
            ImGui::Spacing();

            static int bodyType = 2;
            const char* bodyTypes[] = {"Static", "Kinematic", "Dynamic"};
            ImGui::Combo("Body Type", &bodyType, bodyTypes, 3);

            static float mass = 1.0f;
            ImGui::DragFloat("Mass", &mass, 0.1f, 0.0f, 1000.0f);

            static float gravityScale = 1.0f;
            ImGui::DragFloat("Gravity Scale", &gravityScale, 0.1f, -10.0f, 10.0f);

            static float linearDamping = 0.0f;
            ImGui::DragFloat("Linear Damping", &linearDamping, 0.01f, 0.0f, 10.0f);

            static float angularDamping = 0.05f;
            ImGui::DragFloat("Angular Damping", &angularDamping, 0.01f, 0.0f, 10.0f);

            static float friction = 0.3f;
            ImGui::DragFloat("Friction", &friction, 0.01f, 0.0f, 1.0f);

            static float restitution = 0.0f;
            ImGui::DragFloat("Restitution", &restitution, 0.01f, 0.0f, 1.0f);

            static bool fixedRotation = false;
            ImGui::Checkbox("Fixed Rotation", &fixedRotation);

            static bool isBullet = false;
            ImGui::Checkbox("Bullet (CCD)", &isBullet);

            ImGui::Separator();

            // Collider2D properties
            ImGui::Text(ICON_FA_SHAPES " Collider 2D");
            static int shape = 0;
            const char* shapes[] = {"Box", "Circle", "Capsule", "Polygon", "Edge"};
            ImGui::Combo("Shape", &shape, shapes, 5);

            if (shape == 0)
            {
                static float halfExtents[2] = {0.5f, 0.5f};
                ImGui::DragFloat2("Half Extents", halfExtents, 0.01f, 0.01f, 100.0f);
            }
            else if (shape == 1)
            {
                static float radius = 0.5f;
                ImGui::DragFloat("Radius", &radius, 0.01f, 0.01f, 100.0f);
            }
            else if (shape == 2)
            {
                static float capRadius = 0.5f;
                static float capHeight = 1.0f;
                ImGui::DragFloat("Radius", &capRadius, 0.01f, 0.01f, 100.0f);
                ImGui::DragFloat("Height", &capHeight, 0.01f, 0.01f, 100.0f);
            }

            static float offset[2] = {0.0f, 0.0f};
            ImGui::DragFloat2("Offset", offset, 0.01f, -100.0f, 100.0f);

            static bool isTrigger = false;
            ImGui::Checkbox("Is Trigger", &isTrigger);
        }
    }

    void Physics2DPanel::RenderRaycastTool()
    {
        if (ImGui::CollapsingHeader(ICON_FA_CROSSHAIRS " Raycast Tool"))
        {
            ImGui::DragFloat2("Origin", m_rayOrigin, 0.1f);
            ImGui::DragFloat2("Direction", m_rayDirection, 0.1f);
            ImGui::DragFloat("Max Distance", &m_rayMaxDist, 1.0f, 0.1f, 10000.0f);

            if (ImGui::Button("Cast Ray"))
            {
                // Would cast ray through Physics2DWorld
                ImGui::Text("Ray cast result: (no physics world connected)");
            }

            ImGui::SameLine();
            if (ImGui::Button("Cast from Mouse"))
            {
                ImGui::Text("Click in scene view to set ray origin");
            }
        }
    }

} // namespace SparkEditor
