/**
 * @file ParticleEditorPanel.cpp
 * @brief Implementation of the particle system editor panel
 */

#include "ParticleEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

namespace SparkEditor
{

    ParticleEditorPanel::ParticleEditorPanel() : EditorPanel("Particle Editor", "particle_editor_panel") {}

    bool ParticleEditorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ParticleEditorPanel initialized");
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

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_UNDO " Reset"))
            {
                // Reset all values to defaults
                m_emissionRate = 10.0f;
                m_maxParticles = 1000;
                m_burstCount = 0;
                m_shape = 0;
                m_shapeRadius = 1.0f;
                m_lifetimeMin = 1.0f;
                m_lifetimeMax = 2.0f;
                m_speedMin = 1.0f;
                m_speedMax = 5.0f;
                m_sizeMin = 0.1f;
                m_sizeMax = 0.5f;
                m_gravityMultiplier = 0.0f;
                m_drag = 0.0f;
            }

            RenderPresetSelector();
            ImGui::Separator();

            RenderEmissionSettings();
            RenderShapeSettings();
            RenderAppearanceSettings();
            RenderPhysicsSettings();
            RenderSubEmitterSettings();
            RenderTextureAnimSettings();
            RenderNoiseSettings();
            RenderTrailSettings();
            RenderRenderingSettings();
        }
        EndPanel();
    }

    void ParticleEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "ParticleEditorPanel shutting down");
    }

    void ParticleEditorPanel::RenderPresetSelector()
    {
        ImGui::Text(ICON_FA_BOOKMARK " Presets:");
        ImGui::SameLine();

        if (ImGui::SmallButton("Fire"))
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "ParticleEditorPanel: applied 'Fire' preset");
            m_emissionRate = 50.0f;
            m_lifetimeMin = 0.3f;
            m_lifetimeMax = 1.0f;
            m_speedMin = 2.0f;
            m_speedMax = 5.0f;
            m_sizeMin = 0.2f;
            m_sizeMax = 0.8f;
            m_startColor = {1.0f, 0.5f, 0.0f, 1.0f};
            m_endColor = {1.0f, 0.0f, 0.0f, 0.0f};
            m_gravityMultiplier = -0.5f;
            m_blendMode = 0;
            m_shape = 1;
            m_shapeRadius = 0.3f;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Smoke"))
        {
            m_emissionRate = 20.0f;
            m_lifetimeMin = 2.0f;
            m_lifetimeMax = 4.0f;
            m_speedMin = 0.5f;
            m_speedMax = 2.0f;
            m_sizeMin = 0.5f;
            m_sizeMax = 2.0f;
            m_startColor = {0.5f, 0.5f, 0.5f, 0.5f};
            m_endColor = {0.3f, 0.3f, 0.3f, 0.0f};
            m_gravityMultiplier = -0.2f;
            m_blendMode = 1;
            m_shape = 1;
            m_shapeRadius = 0.5f;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Sparks"))
        {
            m_emissionRate = 100.0f;
            m_lifetimeMin = 0.5f;
            m_lifetimeMax = 1.5f;
            m_speedMin = 5.0f;
            m_speedMax = 15.0f;
            m_sizeMin = 0.02f;
            m_sizeMax = 0.05f;
            m_startColor = {1.0f, 0.9f, 0.3f, 1.0f};
            m_endColor = {1.0f, 0.3f, 0.0f, 0.0f};
            m_gravityMultiplier = 1.0f;
            m_blendMode = 0;
            m_shape = 2;
            m_shapeRadius = 0.1f;
            m_coneAngle = 30.0f;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rain"))
        {
            m_emissionRate = 500.0f;
            m_lifetimeMin = 1.0f;
            m_lifetimeMax = 2.0f;
            m_speedMin = 10.0f;
            m_speedMax = 15.0f;
            m_sizeMin = 0.01f;
            m_sizeMax = 0.02f;
            m_startColor = {0.7f, 0.8f, 1.0f, 0.6f};
            m_endColor = {0.7f, 0.8f, 1.0f, 0.0f};
            m_gravityMultiplier = 2.0f;
            m_blendMode = 1;
            m_shape = 3;
            m_shapeExtents = {20.0f, 0.1f, 20.0f};
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Snow"))
        {
            m_emissionRate = 200.0f;
            m_lifetimeMin = 3.0f;
            m_lifetimeMax = 6.0f;
            m_speedMin = 0.5f;
            m_speedMax = 1.5f;
            m_sizeMin = 0.03f;
            m_sizeMax = 0.08f;
            m_startColor = {1.0f, 1.0f, 1.0f, 0.8f};
            m_endColor = {1.0f, 1.0f, 1.0f, 0.0f};
            m_gravityMultiplier = 0.3f;
            m_drag = 2.0f;
            m_blendMode = 1;
            m_shape = 3;
            m_shapeExtents = {20.0f, 0.1f, 20.0f};
        }
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

    void ParticleEditorPanel::RenderSubEmitterSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Sub-Emitters"))
        {
            ImGui::Indent(4);

            ImGui::Checkbox("Emit on Birth", &m_subEmitOnBirth);
            ImGui::SameLine();
            ImGui::Checkbox("Emit on Death", &m_subEmitOnDeath);
            ImGui::SameLine();
            ImGui::Checkbox("Emit on Collision", &m_subEmitOnCollision);

            if (m_subEmitOnBirth || m_subEmitOnDeath || m_subEmitOnCollision)
            {
                ImGui::DragInt("Sub-emit Count", &m_subEmitCount, 1.0f, 1, 100);
                ImGui::TextDisabled("Sub-emitters inherit parent effect settings by default.");
            }

            ImGui::Unindent(4);
        }
    }

    void ParticleEditorPanel::RenderTextureAnimSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_TH " Texture Sheet Animation"))
        {
            ImGui::Indent(4);

            ImGui::Checkbox("Enable Texture Animation", &m_useTextureAnim);
            if (m_useTextureAnim)
            {
                ImGui::DragInt("Rows", &m_texAnimRows, 1.0f, 1, 16);
                ImGui::DragInt("Columns", &m_texAnimCols, 1.0f, 1, 16);
                ImGui::DragFloat("Frame Rate", &m_texAnimFrameRate, 0.5f, 0.1f, 120.0f, "%.1f fps");
                ImGui::Text("Total Frames: %d", m_texAnimRows * m_texAnimCols);
            }

            ImGui::Unindent(4);
        }
    }

    void ParticleEditorPanel::RenderNoiseSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_WAVE_SQUARE " Noise"))
        {
            ImGui::Indent(4);

            ImGui::Checkbox("Enable Noise", &m_useNoise);
            if (m_useNoise)
            {
                ImGui::DragFloat("Strength", &m_noiseStrength, 0.1f, 0.0f, 50.0f);
                ImGui::DragFloat("Frequency", &m_noiseFrequency, 0.1f, 0.01f, 10.0f);
                ImGui::DragFloat("Scroll Speed", &m_noiseScrollSpeed, 0.1f, 0.0f, 10.0f);
            }

            ImGui::Unindent(4);
        }
    }

    void ParticleEditorPanel::RenderTrailSettings()
    {
        if (ImGui::CollapsingHeader(ICON_FA_ROUTE " Trails"))
        {
            ImGui::Indent(4);

            ImGui::Checkbox("Enable Trails", &m_useTrails);
            if (m_useTrails)
            {
                ImGui::DragFloat("Width", &m_trailWidth, 0.01f, 0.001f, 5.0f, "%.3f");
                ImGui::DragFloat("Lifetime", &m_trailLifetime, 0.1f, 0.01f, 10.0f, "%.2f s");
                ImGui::DragInt("Min Vertex Distance", &m_trailMinVertexDistance, 1.0f, 1, 100);
            }

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
