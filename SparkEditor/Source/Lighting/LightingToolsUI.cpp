/**
 * @file LightingToolsUI.cpp
 * @brief UI rendering methods for LightingTools
 *
 * Split from LightingTools.cpp for maintainability.
 */

#include "LightingTools.h"
#include <imgui.h>
#include <algorithm>
#include <sstream>
#include <cmath>

namespace SparkEditor
{

    void LightingTools::RenderLightListUI()
    {
        if (ImGui::Button("Add Point Light"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Point;
            newLight.name = "Point Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Spot Light"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Spot;
            newLight.name = "Spot Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Directional"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Directional;
            newLight.name = "Directional Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }
        ImGui::SameLine();
        if (ImGui::Button("Add Area Light"))
        {
            SparkLightData newLight;
            newLight.type = SparkLightType::Area;
            newLight.name = "Area Light " + std::to_string(m_nextLightId);
            m_selectedLightId = CreateLight(newLight);
        }

        ImGui::Separator();

        // Light list
        if (ImGui::BeginChild("LightList", ImVec2(0, 300), true))
        {
            for (auto& [id, light] : m_lights)
            {
                const char* typeIcon = "";
                switch (light.type)
                {
                case SparkLightType::Directional:
                    typeIcon = "[D]";
                    break;
                case SparkLightType::Point:
                    typeIcon = "[P]";
                    break;
                case SparkLightType::Spot:
                    typeIcon = "[S]";
                    break;
                case SparkLightType::Area:
                    typeIcon = "[A]";
                    break;
                case SparkLightType::Environment:
                    typeIcon = "[E]";
                    break;
                case SparkLightType::Volumetric:
                    typeIcon = "[V]";
                    break;
                }

                std::string label = std::string(typeIcon) + " " + light.name;
                if (!light.isActive)
                {
                    label += " (disabled)";
                }

                bool isSelected = (m_selectedLightId == id);
                if (ImGui::Selectable(label.c_str(), isSelected))
                {
                    m_selectedLightId = id;
                }
            }
        }
        ImGui::EndChild();

        // Delete button
        if (m_selectedLightId != 0)
        {
            if (ImGui::Button("Delete Selected"))
            {
                DeleteLight(m_selectedLightId);
            }
        }

        ImGui::Text("Total lights: %d", static_cast<int>(m_lights.size()));
    }

    void LightingTools::RenderLightInspectorUI()
    {
        if (m_selectedLightId == 0)
        {
            ImGui::TextDisabled("No light selected. Select a light from the Lights tab.");
            return;
        }

        auto it = m_lights.find(m_selectedLightId);
        if (it == m_lights.end())
        {
            ImGui::TextDisabled("Selected light not found.");
            m_selectedLightId = 0;
            return;
        }

        SparkLightData& light = it->second;
        bool changed = false;

        // Name
        char nameBuffer[256];
        std::strncpy(nameBuffer, light.name.c_str(), sizeof(nameBuffer) - 1);
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            light.name = nameBuffer;
            changed = true;
        }

        // Active toggle
        if (ImGui::Checkbox("Active", &light.isActive))
        {
            changed = true;
        }

        // Light type
        const char* typeNames[] = {"Directional", "Point", "Spot", "Area", "Environment", "Volumetric"};
        int currentType = static_cast<int>(light.type);
        if (ImGui::Combo("Type", &currentType, typeNames, IM_ARRAYSIZE(typeNames)))
        {
            light.type = static_cast<SparkLightType>(currentType);
            changed = true;
        }

        ImGui::Separator();

        // Basic properties
        if (ImGui::TreeNodeEx("Basic Properties", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (light.type != SparkLightType::Directional)
            {
                changed |= ImGui::DragFloat3("Position", &light.position.x, 0.1f);
            }

            if (light.type == SparkLightType::Directional || light.type == SparkLightType::Spot)
            {
                changed |= ImGui::DragFloat3("Direction", &light.direction.x, 0.01f, -1.0f, 1.0f);
            }

            changed |= ImGui::ColorEdit3("Color", &light.color.x);
            changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.01f, 0.0f, 100.0f);

            if (light.type != SparkLightType::Directional)
            {
                changed |= ImGui::DragFloat("Range", &light.range, 0.1f, 0.1f, 1000.0f);
            }

            changed |= ImGui::DragFloat("Temperature (K)", &light.temperature, 10.0f, 1000.0f, 20000.0f);

            ImGui::TreePop();
        }

        // Spot light properties
        if (light.type == SparkLightType::Spot)
        {
            if (ImGui::TreeNodeEx("Spot Light", ImGuiTreeNodeFlags_DefaultOpen))
            {
                changed |= ImGui::SliderFloat("Inner Cone Angle", &light.innerConeAngle, 1.0f, 89.0f);
                changed |= ImGui::SliderFloat("Outer Cone Angle", &light.outerConeAngle, 1.0f, 90.0f);

                // Ensure outer >= inner
                if (light.outerConeAngle < light.innerConeAngle)
                {
                    light.outerConeAngle = light.innerConeAngle;
                    changed = true;
                }

                ImGui::TreePop();
            }
        }

        // Area light properties
        if (light.type == SparkLightType::Area)
        {
            if (ImGui::TreeNodeEx("Area Light", ImGuiTreeNodeFlags_DefaultOpen))
            {
                changed |= ImGui::DragFloat2("Size", &light.areaSize.x, 0.1f, 0.01f, 100.0f);
                ImGui::TreePop();
            }
        }

        // Falloff
        if (ImGui::TreeNode("Falloff"))
        {
            const char* falloffNames[] = {"Linear", "Quadratic", "Inverse Square", "Custom"};
            int currentFalloff = static_cast<int>(light.falloffType);
            if (ImGui::Combo("Falloff Type", &currentFalloff, falloffNames, IM_ARRAYSIZE(falloffNames)))
            {
                light.falloffType = static_cast<LightFalloff>(currentFalloff);
                changed = true;
            }

            if (light.falloffType == LightFalloff::Custom)
            {
                changed |= ImGui::DragFloat("Falloff Exponent", &light.falloffExponent, 0.1f, 0.1f, 10.0f);
            }

            ImGui::TreePop();
        }

        // Shadows
        if (ImGui::TreeNode("Shadows"))
        {
            changed |= ImGui::Checkbox("Cast Shadows", &light.castShadows);

            if (light.castShadows)
            {
                const char* shadowNames[] = {"Disabled", "Low", "Medium", "High", "Ultra", "RTX"};
                int currentShadow = static_cast<int>(light.shadowQuality);
                if (ImGui::Combo("Shadow Quality", &currentShadow, shadowNames, IM_ARRAYSIZE(shadowNames)))
                {
                    light.shadowQuality = static_cast<ShadowQuality>(currentShadow);
                    changed = true;
                }

                changed |= ImGui::DragFloat("Shadow Bias", &light.shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f");
                changed |= ImGui::DragFloat("Normal Bias", &light.shadowNormalBias, 0.001f, 0.0f, 1.0f, "%.3f");
                changed |= ImGui::DragFloat("Shadow Distance", &light.shadowDistance, 1.0f, 1.0f, 1000.0f);

                if (light.type == SparkLightType::Directional)
                {
                    changed |= ImGui::SliderInt("Cascades", &light.shadowCascades, 1, 8);
                }
            }

            ImGui::TreePop();
        }

        // Volumetric
        if (ImGui::TreeNode("Volumetrics"))
        {
            changed |= ImGui::Checkbox("Enable Volumetrics", &light.enableVolumetrics);

            if (light.enableVolumetrics)
            {
                changed |= ImGui::SliderFloat("Volumetric Strength", &light.volumetricStrength, 0.0f, 5.0f);
                changed |= ImGui::SliderFloat("Volumetric Density", &light.volumetricDensity, 0.001f, 1.0f);
            }

            ImGui::TreePop();
        }

        // Performance
        if (ImGui::TreeNode("Performance"))
        {
            changed |= ImGui::Checkbox("Affect Transparency", &light.affectTransparency);
            changed |= ImGui::DragFloat("Culling Radius", &light.cullingRadius, 0.5f, -1.0f, 500.0f);
            changed |= ImGui::DragInt("Max Affected Objects", &light.maxAffectedObjects, 1.0f, 1, 4096);
            changed |= ImGui::DragInt("Priority", &light.priority, 1.0f, 0, 100);

            ImGui::TreePop();
        }

        if (changed && m_lightChangedCallback)
        {
            m_lightChangedCallback(light);
        }
    }

    void LightingTools::RenderGlobalIlluminationUI()
    {
        ImGui::Checkbox("Enable Global Illumination", &m_giSettings.enableGI);
        ImGui::Checkbox("Enable SSAO", &m_giSettings.enableSSAO);
        ImGui::Checkbox("Enable SSR", &m_giSettings.enableSSR);
        ImGui::Checkbox("Enable Ray-Traced GI", &m_giSettings.enableRTGI);

        ImGui::Separator();

        if (ImGui::TreeNodeEx("Ambient Lighting", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::ColorEdit3("Ambient Color", &m_giSettings.ambientColor.x);
            ImGui::DragFloat("Ambient Intensity", &m_giSettings.ambientIntensity, 0.01f, 0.0f, 5.0f);

            char skyboxBuffer[512];
            std::strncpy(skyboxBuffer, m_giSettings.skyboxTexture.c_str(), sizeof(skyboxBuffer) - 1);
            skyboxBuffer[sizeof(skyboxBuffer) - 1] = '\0';
            if (ImGui::InputText("Skybox Texture", skyboxBuffer, sizeof(skyboxBuffer)))
            {
                m_giSettings.skyboxTexture = skyboxBuffer;
            }

            ImGui::DragFloat("Skybox Rotation", &m_giSettings.skyboxRotation, 1.0f, 0.0f, 360.0f);
            ImGui::DragFloat("Skybox Exposure", &m_giSettings.skyboxExposure, 0.01f, 0.01f, 10.0f);

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Light Probes"))
        {
            ImGui::DragInt("Probe Resolution", &m_giSettings.lightProbeResolution, 1.0f, 8, 256);
            ImGui::DragFloat("Probe Spacing", &m_giSettings.lightProbeSpacing, 0.1f, 0.5f, 50.0f);
            ImGui::DragInt("Max Probes", &m_giSettings.maxLightProbes, 1.0f, 1, 10000);

            if (ImGui::Button("Generate Light Probes"))
            {
                XMFLOAT3 defaultBounds = {100.0f, 50.0f, 100.0f};
                int count = GenerateLightProbes(defaultBounds, m_giSettings.lightProbeSpacing);
                m_metrics.lightProbes = count;
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Lightmap Baking"))
        {
            ImGui::DragInt("Lightmap Resolution", &m_giSettings.lightmapResolution, 1.0f, 64, 4096);
            ImGui::DragFloat("UV Padding", &m_giSettings.lightmapPadding, 0.1f, 0.0f, 16.0f);
            ImGui::Checkbox("Use Denoising", &m_giSettings.useDenoising);
            ImGui::DragInt("Bounce Count", &m_giSettings.bounceCount, 1.0f, 1, 16);

            if (m_lightmapBakeInProgress)
            {
                ImGui::ProgressBar(m_bakeProgress);
                ImGui::Text("%s", m_bakeStatus.c_str());
            }
            else
            {
                if (ImGui::Button("Bake Lightmaps"))
                {
                    BakeLightmaps(
                        [this](float progress, const std::string& status)
                        {
                            m_bakeProgress = progress;
                            m_bakeStatus = status;
                        });
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Baked Data"))
                {
                    ClearBakedLighting();
                }
            }

            ImGui::TreePop();
        }
    }

    void LightingTools::RenderAtmosphereUI()
    {
        // Time of day
        if (ImGui::TreeNodeEx("Time of Day", ImGuiTreeNodeFlags_DefaultOpen))
        {
            float time = m_atmosphereSettings.timeOfDay;
            if (ImGui::SliderFloat("Time", &time, 0.0f, 24.0f, "%.2f h"))
            {
                SetTimeOfDay(time);
            }

            int hours = static_cast<int>(time);
            int minutes = static_cast<int>((time - hours) * 60.0f);
            ImGui::Text("Time: %02d:%02d", hours, minutes);

            bool animate = m_animateTimeOfDay;
            if (ImGui::Checkbox("Animate Time of Day", &animate))
            {
                SetTimeOfDayAnimation(animate, m_atmosphereSettings.dayDuration);
            }

            if (m_animateTimeOfDay)
            {
                ImGui::DragFloat("Day Duration (s)", &m_atmosphereSettings.dayDuration, 1.0f, 10.0f, 3600.0f);
                ImGui::DragFloat("Animation Speed", &m_timeOfDaySpeed, 0.1f, 0.1f, 10.0f);
            }

            ImGui::TreePop();
        }

        // Sun/Moon
        if (ImGui::TreeNodeEx("Sun / Moon", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat3("Sun Direction", &m_atmosphereSettings.sunDirection.x, 0.01f, -1.0f, 1.0f);
            ImGui::ColorEdit3("Sun Color", &m_atmosphereSettings.sunColor.x);
            ImGui::DragFloat("Sun Intensity", &m_atmosphereSettings.sunIntensity, 0.1f, 0.0f, 20.0f);
            ImGui::DragFloat("Sun Angular Size", &m_atmosphereSettings.sunAngularSize, 0.01f, 0.1f, 5.0f);

            ImGui::Separator();

            ImGui::DragFloat3("Moon Direction", &m_atmosphereSettings.moonDirection.x, 0.01f, -1.0f, 1.0f);
            ImGui::ColorEdit3("Moon Color", &m_atmosphereSettings.moonColor.x);
            ImGui::DragFloat("Moon Intensity", &m_atmosphereSettings.moonIntensity, 0.01f, 0.0f, 2.0f);

            ImGui::TreePop();
        }

        // Atmosphere scattering
        if (ImGui::TreeNode("Atmosphere Scattering"))
        {
            ImGui::Checkbox("Enable Scattering", &m_atmosphereSettings.enableAtmosphereScattering);
            ImGui::DragFloat3("Rayleigh Scattering", &m_atmosphereSettings.rayleighScattering.x, 0.0001f, 0.0f, 0.1f,
                              "%.4f");
            ImGui::DragFloat("Mie Scattering", &m_atmosphereSettings.mieScattering, 0.0001f, 0.0f, 0.1f, "%.4f");
            ImGui::DragFloat("Turbidity", &m_atmosphereSettings.turbidity, 0.1f, 1.0f, 10.0f);

            ImGui::TreePop();
        }

        // Fog
        if (ImGui::TreeNode("Fog"))
        {
            ImGui::Checkbox("Enable Fog", &m_atmosphereSettings.enableFog);

            if (m_atmosphereSettings.enableFog)
            {
                ImGui::ColorEdit3("Fog Color", &m_atmosphereSettings.fogColor.x);
                ImGui::DragFloat("Fog Density", &m_atmosphereSettings.fogDensity, 0.001f, 0.0f, 1.0f);
                ImGui::DragFloat("Fog Start Distance", &m_atmosphereSettings.fogStartDistance, 1.0f, 0.0f, 1000.0f);
                ImGui::DragFloat("Fog End Distance", &m_atmosphereSettings.fogEndDistance, 1.0f, 0.0f, 5000.0f);
            }

            ImGui::TreePop();
        }

        // Clouds
        if (ImGui::TreeNode("Clouds"))
        {
            ImGui::Checkbox("Enable Clouds", &m_atmosphereSettings.enableClouds);

            if (m_atmosphereSettings.enableClouds)
            {
                ImGui::SliderFloat("Cloud Coverage", &m_atmosphereSettings.cloudCoverage, 0.0f, 1.0f);
                ImGui::SliderFloat("Cloud Density", &m_atmosphereSettings.cloudDensity, 0.0f, 2.0f);
                ImGui::DragFloat2("Wind Direction", &m_atmosphereSettings.cloudWindDirection.x, 0.01f, -1.0f, 1.0f);
                ImGui::DragFloat("Wind Speed", &m_atmosphereSettings.cloudWindSpeed, 0.01f, 0.0f, 5.0f);
            }

            ImGui::TreePop();
        }

        // Weather
        if (ImGui::TreeNode("Weather"))
        {
            ImGui::SliderFloat("Rain Intensity", &m_atmosphereSettings.rainIntensity, 0.0f, 1.0f);
            ImGui::SliderFloat("Snow Intensity", &m_atmosphereSettings.snowIntensity, 0.0f, 1.0f);
            ImGui::DragFloat("Wind Strength", &m_atmosphereSettings.windStrength, 0.01f, 0.0f, 5.0f);
            ImGui::DragFloat3("Wind Direction", &m_atmosphereSettings.windDirection.x, 0.01f, -1.0f, 1.0f);

            ImGui::TreePop();
        }
    }

    void LightingTools::RenderPostProcessingUI()
    {
        // Tonemapping
        if (ImGui::TreeNodeEx("Tonemapping", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Enable Tonemapping", &m_postProcessingSettings.enableTonemapping);

            if (m_postProcessingSettings.enableTonemapping)
            {
                const char* operators[] = {"ACES", "Reinhard", "Filmic", "Uncharted2", "Linear"};
                int currentOp = 0;
                for (int i = 0; i < IM_ARRAYSIZE(operators); ++i)
                {
                    if (m_postProcessingSettings.tonemappingOperator == operators[i])
                    {
                        currentOp = i;
                        break;
                    }
                }
                if (ImGui::Combo("Operator", &currentOp, operators, IM_ARRAYSIZE(operators)))
                {
                    m_postProcessingSettings.tonemappingOperator = operators[currentOp];
                }

                ImGui::DragFloat("Exposure", &m_postProcessingSettings.exposure, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("Gamma", &m_postProcessingSettings.gamma, 0.01f, 1.0f, 4.0f);
            }

            ImGui::TreePop();
        }

        // Color grading
        if (ImGui::TreeNode("Color Grading"))
        {
            ImGui::Checkbox("Enable Color Grading", &m_postProcessingSettings.enableColorGrading);

            if (m_postProcessingSettings.enableColorGrading)
            {
                ImGui::DragFloat("Contrast", &m_postProcessingSettings.contrast, 0.01f, 0.0f, 3.0f);
                ImGui::DragFloat("Saturation", &m_postProcessingSettings.saturation, 0.01f, 0.0f, 3.0f);
                ImGui::DragFloat("Brightness", &m_postProcessingSettings.brightness, 0.01f, -1.0f, 1.0f);
                ImGui::ColorEdit3("Color Tint", &m_postProcessingSettings.colorTint.x);
            }

            ImGui::TreePop();
        }

        // Bloom
        if (ImGui::TreeNode("Bloom"))
        {
            ImGui::Checkbox("Enable Bloom", &m_postProcessingSettings.enableBloom);

            if (m_postProcessingSettings.enableBloom)
            {
                ImGui::DragFloat("Threshold", &m_postProcessingSettings.bloomThreshold, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Intensity", &m_postProcessingSettings.bloomIntensity, 0.01f, 0.0f, 5.0f);
                ImGui::DragFloat("Radius", &m_postProcessingSettings.bloomRadius, 0.01f, 0.1f, 10.0f);
            }

            ImGui::TreePop();
        }

        // Other effects
        if (ImGui::TreeNode("Effects"))
        {
            ImGui::Checkbox("Motion Blur", &m_postProcessingSettings.enableMotionBlur);
            ImGui::Checkbox("Depth of Field", &m_postProcessingSettings.enableDepthOfField);
            ImGui::Checkbox("Chromatic Aberration", &m_postProcessingSettings.enableChromaticAberration);
            ImGui::Checkbox("Vignette", &m_postProcessingSettings.enableVignette);

            ImGui::TreePop();
        }
    }

    void LightingTools::RenderPerformanceUI()
    {
        LightingMetrics metrics = GetLightingMetrics();

        ImGui::Text("Active Lights: %d", metrics.activeLights);
        ImGui::Text("Shadow-Casting Lights: %d", metrics.shadowCastingLights);
        ImGui::Text("Lightmap Textures: %d", metrics.lightmapTextures);
        ImGui::Text("Light Probes: %d", metrics.lightProbes);

        ImGui::Separator();

        float shadowMemMB = static_cast<float>(metrics.shadowMapMemory) / (1024.0f * 1024.0f);
        float lightmapMemMB = static_cast<float>(metrics.lightmapMemory) / (1024.0f * 1024.0f);
        ImGui::Text("Shadow Map Memory: %.2f MB", shadowMemMB);
        ImGui::Text("Lightmap Memory: %.2f MB", lightmapMemMB);
        ImGui::Text("Total Lighting Memory: %.2f MB", shadowMemMB + lightmapMemMB);

        ImGui::Separator();

        ImGui::Text("Render Time: %.3f ms", metrics.renderTime);
        ImGui::Text("Shadow Render Time: %.3f ms", metrics.shadowRenderTime);

        ImGui::Separator();

        static float targetFPS = 60.0f;
        ImGui::DragFloat("Target FPS", &targetFPS, 1.0f, 30.0f, 240.0f);
        if (ImGui::Button("Optimize Lighting"))
        {
            OptimizeLightingPerformance(targetFPS);
        }
    }

    void LightingTools::RenderPresetsUI()
    {
        ImGui::Text("Built-in Presets:");
        ImGui::Separator();

        const char* presets[] = {"Sunny Day", "Sunset", "Night", "Overcast", "Studio"};
        for (const char* preset : presets)
        {
            if (ImGui::Button(preset))
            {
                ApplyLightingPreset(preset);
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();

        ImGui::Separator();
        ImGui::Text("Profiles:");

        // Save profile
        static char profileNameBuffer[256] = "";
        ImGui::InputText("Profile Name", profileNameBuffer, sizeof(profileNameBuffer));
        ImGui::SameLine();
        if (ImGui::Button("Save"))
        {
            if (std::strlen(profileNameBuffer) > 0)
            {
                SaveLightingProfile(profileNameBuffer);
            }
        }

        ImGui::Separator();

        // List available profiles
        std::vector<std::string> profiles = GetAvailableLightingProfiles();
        if (profiles.empty())
        {
            ImGui::TextDisabled("No saved profiles found.");
        }
        else
        {
            for (const auto& profile : profiles)
            {
                if (ImGui::Selectable(profile.c_str()))
                {
                    LoadLightingProfile(profile);
                }
            }
        }
    }

    // =========================================================================
    // Private Helper Methods
    // =========================================================================

    void LightingTools::UpdateSunPosition()
    {
        float time = m_atmosphereSettings.timeOfDay;

        // Convert time of day to sun angle
        // At 6:00 the sun is at the horizon (elevation = 0), at 12:00 it is at zenith
        // Sun travels 180 degrees from 6:00 to 18:00
        float hourAngle = (time - 12.0f) * (static_cast<float>(M_PI) / 12.0f); // radians from noon

        // Elevation: peak at noon (pi/2), horizon at 6 and 18
        float elevation = static_cast<float>(M_PI) * 0.5f - std::abs(hourAngle);

        // Clamp elevation for night time (sun below horizon)
        float azimuth = (time < 12.0f) ? static_cast<float>(M_PI) : 0.0f;

        // Spherical to Cartesian (Y-up coordinate system)
        float cosElev = std::cos(elevation);
        float sinElev = std::sin(elevation);
        float cosAz = std::cos(azimuth);
        float sinAz = std::sin(azimuth);

        // Sun direction points FROM the sun toward the scene (negative because it's a light direction)
        m_atmosphereSettings.sunDirection.x = -cosElev * sinAz;
        m_atmosphereSettings.sunDirection.y = -sinElev;
        m_atmosphereSettings.sunDirection.z = -cosElev * cosAz;

        // Update sun color based on time of day
        m_atmosphereSettings.sunColor = CalculateSunColor(time);

        // Adjust sun intensity based on elevation
        if (sinElev > 0.0f)
        {
            m_atmosphereSettings.sunIntensity = 3.0f * std::clamp(sinElev, 0.0f, 1.0f);
        }
        else
        {
            m_atmosphereSettings.sunIntensity = 0.0f;
        }

        // Update moon direction (opposite hemisphere offset)
        m_atmosphereSettings.moonDirection.x = -m_atmosphereSettings.sunDirection.x;
        m_atmosphereSettings.moonDirection.y = -m_atmosphereSettings.sunDirection.y;
        m_atmosphereSettings.moonDirection.z = -m_atmosphereSettings.sunDirection.z;
    }

    XMFLOAT3 LightingTools::CalculateSunColor(float timeOfDay) const
    {
        // Normalize to 0-1 within the daylight window
        // Sunrise ~6, sunset ~18
        float sunriseStart = 5.0f;
        float sunriseEnd = 7.0f;
        float sunsetStart = 17.0f;
        float sunsetEnd = 19.0f;

        // Night - deep blue twilight
        if (timeOfDay < sunriseStart || timeOfDay > sunsetEnd)
        {
            return {0.1f, 0.1f, 0.3f};
        }

        // Sunrise transition: warm orange/red fading to white
        if (timeOfDay >= sunriseStart && timeOfDay <= sunriseEnd)
        {
            float t = (timeOfDay - sunriseStart) / (sunriseEnd - sunriseStart);
            float r = 1.0f;
            float g = 0.4f + 0.55f * t;
            float b = 0.2f + 0.7f * t;
            return {r, g, b};
        }

        // Sunset transition: white fading to warm orange/red
        if (timeOfDay >= sunsetStart && timeOfDay <= sunsetEnd)
        {
            float t = (timeOfDay - sunsetStart) / (sunsetEnd - sunsetStart);
            float r = 1.0f;
            float g = 0.95f - 0.55f * t;
            float b = 0.9f - 0.7f * t;
            return {r, g, b};
        }

        // Daytime: slightly warm white
        // Bluer in the morning, whiter at noon, slightly warmer in afternoon
        float midday = 12.0f;
        float distFromNoon = std::abs(timeOfDay - midday);
        float warmth = distFromNoon / 5.0f; // 0 at noon, ~1 at edges

        float r = 1.0f;
        float g = 0.98f - 0.03f * warmth;
        float b = 0.95f - 0.05f * warmth;

        return {r, g, b};
    }

    void LightingTools::UpdateAtmosphereScattering()
    {
        if (!m_atmosphereSettings.enableAtmosphereScattering)
        {
            return;
        }

        float turbidity = m_atmosphereSettings.turbidity;

        // Rayleigh scattering coefficients scale with turbidity
        // Base values correspond to Earth's atmosphere at sea level
        float rayleighScale = 1.0f + (turbidity - 2.0f) * 0.1f;
        m_atmosphereSettings.rayleighScattering.x = 0.0025f * rayleighScale;
        m_atmosphereSettings.rayleighScattering.y = 0.0041f * rayleighScale;
        m_atmosphereSettings.rayleighScattering.z = 0.0081f * rayleighScale;

        // Mie scattering increases more strongly with turbidity (aerosols/haze)
        float mieScale = 1.0f + (turbidity - 2.0f) * 0.3f;
        m_atmosphereSettings.mieScattering = 0.003f * mieScale;

        // Adjust fog color to match atmosphere at horizon
        if (m_atmosphereSettings.enableFog)
        {
            XMFLOAT3 sunCol = m_atmosphereSettings.sunColor;
            float sunElev = -m_atmosphereSettings.sunDirection.y; // higher Y means more overhead sun
            float horizonBlend = std::clamp(1.0f - sunElev, 0.0f, 1.0f);

            // Fog picks up sun color near horizon, stays base color overhead
            m_atmosphereSettings.fogColor.x = 0.7f * (1.0f - horizonBlend) + sunCol.x * 0.3f * horizonBlend;
            m_atmosphereSettings.fogColor.y = 0.8f * (1.0f - horizonBlend) + sunCol.y * 0.3f * horizonBlend;
            m_atmosphereSettings.fogColor.z = 0.9f * (1.0f - horizonBlend) + sunCol.z * 0.3f * horizonBlend;
        }
    }

    bool LightingTools::ValidateLightData(const SparkLightData& lightData) const
    {
        // Intensity must be non-negative
        if (lightData.intensity < 0.0f)
        {
            return false;
        }

        // Range must be positive (except for directional lights where range is unused)
        if (lightData.type != SparkLightType::Directional && lightData.range <= 0.0f)
        {
            return false;
        }

        // Cone angles must be valid for spot lights
        if (lightData.type == SparkLightType::Spot)
        {
            if (lightData.innerConeAngle < 0.0f || lightData.innerConeAngle > 90.0f)
            {
                return false;
            }
            if (lightData.outerConeAngle < 0.0f || lightData.outerConeAngle > 90.0f)
            {
                return false;
            }
            if (lightData.outerConeAngle < lightData.innerConeAngle)
            {
                return false;
            }
        }

        // Area size must be positive
        if (lightData.type == SparkLightType::Area)
        {
            if (lightData.areaSize.x <= 0.0f || lightData.areaSize.y <= 0.0f)
            {
                return false;
            }
        }

        // Color channels should be non-negative
        if (lightData.color.x < 0.0f || lightData.color.y < 0.0f || lightData.color.z < 0.0f)
        {
            return false;
        }

        // Temperature must be in reasonable range
        if (lightData.temperature < 1000.0f || lightData.temperature > 20000.0f)
        {
            return false;
        }

        // Shadow bias values should be non-negative
        if (lightData.shadowBias < 0.0f || lightData.shadowNormalBias < 0.0f)
        {
            return false;
        }

        // Shadow distance must be positive
        if (lightData.shadowDistance <= 0.0f)
        {
            return false;
        }

        // Shadow cascades must be in [1, 8]
        if (lightData.shadowCascades < 1 || lightData.shadowCascades > 8)
        {
            return false;
        }

        // Volumetric properties
        if (lightData.enableVolumetrics)
        {
            if (lightData.volumetricStrength < 0.0f)
            {
                return false;
            }
            if (lightData.volumetricDensity < 0.0f)
            {
                return false;
            }
        }

        // Max affected objects must be positive
        if (lightData.maxAffectedObjects < 1)
        {
            return false;
        }

        // Falloff exponent must be positive for custom falloff
        if (lightData.falloffType == LightFalloff::Custom && lightData.falloffExponent <= 0.0f)
        {
            return false;
        }

        return true;
    }


} // namespace SparkEditor
