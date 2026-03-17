/**
 * @file ParticleEditorPanel.cpp
 * @brief Implementation of the particle system editor panel
 */

#include "ParticleEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    ParticleEditorPanel::ParticleEditorPanel() : EditorPanel("Particle Editor", "particle_editor_panel") {}

    bool ParticleEditorPanel::Initialize()
    {
        std::cout << "Initializing Particle Editor panel\n";
        return true;
    }

    void ParticleEditorPanel::Update(float /*deltaTime*/) {}

    void ParticleEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            ImGui::InputText("Effect Name", m_effectName, sizeof(m_effectName));
            ImGui::Separator();

            RenderEmissionSettings();
            RenderShapeSettings();
            RenderAppearanceSettings();
            RenderPhysicsSettings();
            RenderRenderingSettings();
        }
        EndPanel();
    }

    void ParticleEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Particle Editor panel\n";
    }

    void ParticleEditorPanel::RenderEmissionSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_FIRE " Emission", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);
            ImGui::DragFloat("Rate", &m_emissionRate, 1.0f, 0.0f, 10000.0f, "%.0f/s");
            ImGui::DragInt("Max Particles", &m_maxParticles, 10.0f, 1, 100000);
            ImGui::DragInt("Burst Count", &m_burstCount, 1.0f, 0, 1000);
            ImGui::Checkbox("Loop", &m_loop);
            ImGui::SameLine();
            ImGui::Checkbox("Play On Awake", &m_playOnAwake);
            ImGui::SameLine();
            ImGui::Checkbox("Prewarm", &m_prewarm);
            ImGui::Unindent(4);
        }
    }

    void ParticleEditorPanel::RenderShapeSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_SHAPES " Shape", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);
            const char* shapes[] = {"Point", "Sphere", "Cone", "Box", "Circle"};
            ImGui::Combo("Shape", &m_shape, shapes, IM_ARRAYSIZE(shapes));

            if (m_shape == 1 || m_shape == 4) // Sphere or Circle
                ImGui::DragFloat("Radius", &m_shapeRadius, 0.1f, 0.0f, 100.0f);

            if (m_shape == 2) // Cone
            {
                ImGui::DragFloat("Radius", &m_shapeRadius, 0.1f, 0.0f, 100.0f);
                ImGui::DragFloat("Angle", &m_coneAngle, 1.0f, 0.0f, 90.0f);
            }

            if (m_shape == 3) // Box
            {
                float ext[3] = {m_shapeExtents.x, m_shapeExtents.y, m_shapeExtents.z};
                if (ImGui::DragFloat3("Extents", ext, 0.1f, 0.0f))
                    m_shapeExtents = {ext[0], ext[1], ext[2]};
            }
            ImGui::Unindent(4);
        }
    }

    void ParticleEditorPanel::RenderAppearanceSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_PALETTE " Appearance", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);

            ImGui::TextDisabled("Lifetime");
            ImGui::DragFloatRange2("Lifetime", &m_lifetimeMin, &m_lifetimeMax, 0.01f, 0.01f, 60.0f, "%.2f s", "%.2f s");

            ImGui::TextDisabled("Speed");
            ImGui::DragFloatRange2("Speed", &m_speedMin, &m_speedMax, 0.1f, 0.0f, 200.0f);

            ImGui::TextDisabled("Size");
            ImGui::DragFloatRange2("Size", &m_sizeMin, &m_sizeMax, 0.01f, 0.001f, 10.0f);

            ImGui::TextDisabled("Color Over Lifetime");
            float startCol[4] = {m_startColor.x, m_startColor.y, m_startColor.z, m_startColor.w};
            if (ImGui::ColorEdit4("Start Color", startCol))
                m_startColor = {startCol[0], startCol[1], startCol[2], startCol[3]};

            float endCol[4] = {m_endColor.x, m_endColor.y, m_endColor.z, m_endColor.w};
            if (ImGui::ColorEdit4("End Color", endCol))
                m_endColor = {endCol[0], endCol[1], endCol[2], endCol[3]};

            ImGui::Unindent(4);
        }
    }

    void ParticleEditorPanel::RenderPhysicsSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_GLOBE " Physics"))
        {
            ImGui::Indent(4);
            ImGui::DragFloat("Gravity", &m_gravityMultiplier, 0.01f, -5.0f, 5.0f);
            ImGui::DragFloat("Drag", &m_drag, 0.01f, 0.0f, 10.0f);
            ImGui::Unindent(4);
        }
    }

    void ParticleEditorPanel::RenderRenderingSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_EYE " Rendering"))
        {
            ImGui::Indent(4);

            const char* blendModes[] = {"Additive", "Alpha Blend", "Multiply"};
            ImGui::Combo("Blend Mode", &m_blendMode, blendModes, IM_ARRAYSIZE(blendModes));

            const char* spaces[] = {"World", "Local"};
            ImGui::Combo("Space", &m_space, spaces, IM_ARRAYSIZE(spaces));

            ImGui::Unindent(4);
        }
    }

} // namespace SparkEditor
