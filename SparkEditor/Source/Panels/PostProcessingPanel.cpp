/**
 * @file PostProcessingPanel.cpp
 * @brief Implementation of the post-processing settings panel
 */

#include "PostProcessingPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>

namespace SparkEditor
{

    PostProcessingPanel::PostProcessingPanel() : EditorPanel("Post Processing", "postprocessing_panel") {}

    bool PostProcessingPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PostProcessingPanel initialized");
        return true;
    }

    void PostProcessingPanel::Update(float /*deltaTime*/) {}

    void PostProcessingPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (!m_scene)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "PostProcessingPanel: no scene loaded");
                ImGui::TextDisabled("No scene loaded");
            }
            else
            {
                RenderBloomSettings();
                RenderTonemappingSettings();
                RenderFogSettings();
                RenderSkySettings();
                RenderWindSettings();
            }
        }
        EndPanel();
    }

    void PostProcessingPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "PostProcessingPanel shutting down");
    }

    void PostProcessingPanel::RenderBloomSettings()
    {
        auto& env = m_scene->environment;

        if (ImGui::CollapsingHeader(ICON_FA_SUN " Bloom", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);
            ImGui::Checkbox("Enabled##Bloom", &env.bloomEnabled);

            if (env.bloomEnabled)
            {
                ImGui::DragFloat("Intensity", &env.bloomIntensity, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("Threshold", &env.bloomThreshold, 0.01f, 0.0f, 10.0f);
            }
            ImGui::Unindent(4);
        }
    }

    void PostProcessingPanel::RenderTonemappingSettings()
    {
        auto& env = m_scene->environment;

        if (ImGui::CollapsingHeader(ICON_FA_ADJUST " Tonemapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(4);
            ImGui::Checkbox("Enabled##Tonemap", &env.tonemappingEnabled);

            if (env.tonemappingEnabled)
            {
                ImGui::DragFloat("Exposure", &env.exposure, 0.01f, 0.01f, 20.0f);
                ImGui::DragFloat("Gamma", &env.gamma, 0.01f, 0.5f, 5.0f);
            }
            ImGui::Unindent(4);
        }
    }

    void PostProcessingPanel::RenderFogSettings()
    {
        auto& env = m_scene->environment;

        if (ImGui::CollapsingHeader(ICON_FA_SMOG " Fog"))
        {
            ImGui::Indent(4);
            ImGui::Checkbox("Enabled##Fog", &env.fogEnabled);

            if (env.fogEnabled)
            {
                float fogCol[4] = {env.fogColor.x, env.fogColor.y, env.fogColor.z, env.fogColor.w};
                if (ImGui::ColorEdit4("Fog Color", fogCol))
                    env.fogColor = {fogCol[0], fogCol[1], fogCol[2], fogCol[3]};

                ImGui::DragFloat("Density", &env.fogDensity, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Start Distance", &env.fogStart, 1.0f, 0.0f, env.fogEnd);
                ImGui::DragFloat("End Distance", &env.fogEnd, 1.0f, env.fogStart, 10000.0f);
            }
            ImGui::Unindent(4);
        }
    }

    void PostProcessingPanel::RenderSkySettings()
    {
        auto& env = m_scene->environment;

        if (ImGui::CollapsingHeader(ICON_FA_CLOUD " Sky"))
        {
            ImGui::Indent(4);

            const char* skyTypes[] = {"Solid Color", "Gradient", "Skybox", "Procedural"};
            int skyType = static_cast<int>(env.skyType);
            if (ImGui::Combo("Sky Type", &skyType, skyTypes, IM_ARRAYSIZE(skyTypes)))
                env.skyType = static_cast<EnvironmentSettings::SkyType>(skyType);

            float skyCol[4] = {env.skyColor.x, env.skyColor.y, env.skyColor.z, env.skyColor.w};
            if (ImGui::ColorEdit4("Sky Color", skyCol))
                env.skyColor = {skyCol[0], skyCol[1], skyCol[2], skyCol[3]};

            if (env.skyType == EnvironmentSettings::GRADIENT)
            {
                float horCol[4] = {env.horizonColor.x, env.horizonColor.y, env.horizonColor.z, env.horizonColor.w};
                if (ImGui::ColorEdit4("Horizon Color", horCol))
                    env.horizonColor = {horCol[0], horCol[1], horCol[2], horCol[3]};
            }

            if (env.skyType == EnvironmentSettings::SKYBOX)
            {
                char skyboxBuf[256];
                strncpy(skyboxBuf, env.skyboxAssetPath.c_str(), sizeof(skyboxBuf) - 1);
                skyboxBuf[sizeof(skyboxBuf) - 1] = '\0';
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("Skybox", skyboxBuf, sizeof(skyboxBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    env.skyboxAssetPath = skyboxBuf;
            }
            ImGui::Unindent(4);
        }
    }

    void PostProcessingPanel::RenderWindSettings()
    {
        auto& env = m_scene->environment;

        if (ImGui::CollapsingHeader(ICON_FA_WIND " Wind"))
        {
            ImGui::Indent(4);

            float windDir[3] = {env.windDirection.x, env.windDirection.y, env.windDirection.z};
            if (ImGui::DragFloat3("Direction", windDir, 0.1f, -1.0f, 1.0f))
                env.windDirection = {windDir[0], windDir[1], windDir[2]};

            ImGui::DragFloat("Strength", &env.windStrength, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Turbulence", &env.windTurbulence, 0.01f, 0.0f, 1.0f);
            ImGui::Unindent(4);
        }
    }

} // namespace SparkEditor
