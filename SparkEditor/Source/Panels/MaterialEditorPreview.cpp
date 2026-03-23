/**
 * @file MaterialEditorPreview.cpp
 * @brief Preview rendering, render state editing, and default material setup
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains: RenderRenderStateEditor, RenderPreview, LoadDefaultMaterials,
 * PopulateDefaultPBRParameters.
 */

#include "MaterialEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace SparkEditor
{

    // ========================================================================
    // Render State Editor
    // ========================================================================

    void MaterialEditorPanel::RenderRenderStateEditor()
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        if (selected == nullptr)
        {
            return;
        }

        ImGui::Text(ICON_FA_COG " Render State Configuration");
        ImGui::Separator();
        ImGui::Spacing();

        if (selected->isBuiltIn)
        {
            ImGui::BeginDisabled();
        }

        MaterialRenderState& state = selected->renderState;

        // Blend mode
        const char* blendNames[] = {"Opaque", "Alpha Blend", "Additive", "Multiply", "Custom"};
        int currentBlend = static_cast<int>(state.blendMode);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Blend Mode", &currentBlend, blendNames, 5))
        {
            state.blendMode = static_cast<MaterialRenderState::BlendMode>(currentBlend);
            selected->isModified = true;
            SetModified(true);
        }

        // Alpha clip threshold (visible for alpha modes)
        if (state.blendMode == MaterialRenderState::BlendMode::AlphaBlend ||
            state.blendMode == MaterialRenderState::BlendMode::Custom)
        {
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("Alpha Clip Threshold", &state.alphaClipThreshold, 0.0f, 1.0f, "%.2f"))
            {
                selected->isModified = true;
                SetModified(true);
            }
        }

        ImGui::Spacing();

        // Cull mode
        const char* cullNames[] = {"Back", "Front", "None"};
        int currentCull = static_cast<int>(state.cullMode);
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("Cull Mode", &currentCull, cullNames, 3))
        {
            state.cullMode = static_cast<MaterialRenderState::CullMode>(currentCull);
            selected->isModified = true;
            SetModified(true);
        }

        ImGui::Spacing();

        // Depth settings
        if (ImGui::Checkbox("Depth Write", &state.depthWrite))
        {
            selected->isModified = true;
            SetModified(true);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Whether this material writes to the depth buffer");
        }

        if (ImGui::Checkbox("Depth Test", &state.depthTest))
        {
            selected->isModified = true;
            SetModified(true);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Whether this material tests against the depth buffer");
        }

        ImGui::Spacing();

        // Shadow settings
        if (ImGui::Checkbox("Cast Shadows", &state.castShadows))
        {
            selected->isModified = true;
            SetModified(true);
        }

        if (ImGui::Checkbox("Receive Shadows", &state.receiveShadows))
        {
            selected->isModified = true;
            SetModified(true);
        }

        ImGui::Spacing();

        // Render queue
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::InputInt("Render Queue", &state.renderQueue, 100, 500))
        {
            state.renderQueue = std::clamp(state.renderQueue, 0, 5000);
            selected->isModified = true;
            SetModified(true);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Sorting order: 0-999=Background, 1000-1999=Geometry, "
                              "2000-2999=AlphaTest, 3000-3999=Transparent, 4000+=Overlay");
        }

        if (selected->isBuiltIn)
        {
            ImGui::EndDisabled();
        }
    }

    // ========================================================================
    // Preview
    // ========================================================================

    void MaterialEditorPanel::RenderPreview()
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        if (selected == nullptr)
        {
            return;
        }

        ImGui::Text(ICON_FA_EYE " Material Preview");
        ImGui::Separator();

        float previewSize = std::min(ImGui::GetContentRegionAvail().x, 300.0f);
        ImVec2 previewRegion(previewSize, previewSize);

        // Preview area with border
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            cursorPos, ImVec2(cursorPos.x + previewRegion.x, cursorPos.y + previewRegion.y), IM_COL32(30, 30, 30, 255));
        ImGui::GetWindowDrawList()->AddRect(
            cursorPos, ImVec2(cursorPos.x + previewRegion.x, cursorPos.y + previewRegion.y), IM_COL32(80, 80, 80, 255));

        // Simulated preview rendering (draw a stylized representation)
        float centerX = cursorPos.x + previewRegion.x * 0.5f;
        float centerY = cursorPos.y + previewRegion.y * 0.5f;
        float radius = previewRegion.x * 0.35f;

        // Get material color from albedo parameter for visualization
        float albedoR = 0.8f;
        float albedoG = 0.8f;
        float albedoB = 0.8f;
        float metallic = 0.0f;
        float roughness = 0.5f;

        ShaderParameter* albedoParam = selected->FindParameter("albedoColor");
        if (albedoParam != nullptr)
        {
            albedoR = albedoParam->floatValues[0];
            albedoG = albedoParam->floatValues[1];
            albedoB = albedoParam->floatValues[2];
        }

        ShaderParameter* metallicParam = selected->FindParameter("metallic");
        if (metallicParam != nullptr)
        {
            metallic = metallicParam->floatValues[0];
        }

        ShaderParameter* roughnessParam = selected->FindParameter("roughness");
        if (roughnessParam != nullptr)
        {
            roughness = roughnessParam->floatValues[0];
        }

        // Compute a simple lighting model for preview visualization
        float rotAngle = m_previewTime * m_previewRotationSpeed * 0.0174533f; // deg to rad
        float lightX = std::cos(rotAngle);
        float lightY = -0.5f;
        float lightZ = std::sin(rotAngle);
        float lightLen = std::sqrt(lightX * lightX + lightY * lightY + lightZ * lightZ);
        lightX /= lightLen;
        lightY /= lightLen;
        lightZ /= lightLen;

        auto* drawList = ImGui::GetWindowDrawList();

        if (m_previewShape == PreviewShape::Sphere)
        {
            // Draw a sphere with per-pixel-ish shading using concentric filled circles
            int steps = 32;
            for (int i = steps; i >= 0; --i)
            {
                float t = static_cast<float>(i) / static_cast<float>(steps);
                float r = radius * t;

                // Normal at this ring approximated as pointing outward
                float nz = std::sqrt(std::max(0.0f, 1.0f - t * t));
                float ndotl = std::max(0.0f, nz * (-lightY) + t * lightX * 0.5f);

                // Fresnel-like effect for metallic materials
                float fresnel = std::pow(1.0f - nz, 3.0f) * metallic;

                // Roughness affects specular spread
                float specPower = (1.0f - roughness) * 64.0f + 1.0f;
                float specular = std::pow(std::max(0.0f, ndotl), specPower) * (1.0f - roughness);

                float ambient = 0.15f;
                float diffuse = ndotl * 0.7f;
                float finalR = std::clamp(albedoR * (ambient + diffuse) + specular + fresnel * 0.3f, 0.0f, 1.0f);
                float finalG = std::clamp(albedoG * (ambient + diffuse) + specular + fresnel * 0.3f, 0.0f, 1.0f);
                float finalB = std::clamp(albedoB * (ambient + diffuse) + specular + fresnel * 0.3f, 0.0f, 1.0f);

                ImU32 color = IM_COL32(static_cast<int>(finalR * 255.0f), static_cast<int>(finalG * 255.0f),
                                       static_cast<int>(finalB * 255.0f), 255);
                drawList->AddCircleFilled(ImVec2(centerX, centerY), r, color, 48);
            }
        }
        else if (m_previewShape == PreviewShape::Cube)
        {
            // Draw a simplified cube face
            float halfSize = radius * 0.75f;
            float ambient = 0.15f;
            float diffuse = 0.7f;
            float specular = (1.0f - roughness) * 0.3f;
            float finalR = std::clamp(albedoR * (ambient + diffuse) + specular, 0.0f, 1.0f);
            float finalG = std::clamp(albedoG * (ambient + diffuse) + specular, 0.0f, 1.0f);
            float finalB = std::clamp(albedoB * (ambient + diffuse) + specular, 0.0f, 1.0f);

            ImU32 faceColor = IM_COL32(static_cast<int>(finalR * 255.0f), static_cast<int>(finalG * 255.0f),
                                       static_cast<int>(finalB * 255.0f), 255);

            // Front face
            drawList->AddRectFilled(ImVec2(centerX - halfSize, centerY - halfSize),
                                    ImVec2(centerX + halfSize, centerY + halfSize), faceColor);

            // Top face (darker)
            float topShade = 0.7f;
            ImU32 topColor =
                IM_COL32(static_cast<int>(finalR * topShade * 255.0f), static_cast<int>(finalG * topShade * 255.0f),
                         static_cast<int>(finalB * topShade * 255.0f), 255);
            float offset = halfSize * 0.4f;
            ImVec2 topPoly[4] = {
                ImVec2(centerX - halfSize, centerY - halfSize),
                ImVec2(centerX - halfSize + offset, centerY - halfSize - offset),
                ImVec2(centerX + halfSize + offset, centerY - halfSize - offset),
                ImVec2(centerX + halfSize, centerY - halfSize),
            };
            drawList->AddConvexPolyFilled(topPoly, 4, topColor);

            // Right face (darker)
            float rightShade = 0.5f;
            ImU32 rightColor =
                IM_COL32(static_cast<int>(finalR * rightShade * 255.0f), static_cast<int>(finalG * rightShade * 255.0f),
                         static_cast<int>(finalB * rightShade * 255.0f), 255);
            ImVec2 rightPoly[4] = {
                ImVec2(centerX + halfSize, centerY - halfSize),
                ImVec2(centerX + halfSize + offset, centerY - halfSize - offset),
                ImVec2(centerX + halfSize + offset, centerY + halfSize - offset),
                ImVec2(centerX + halfSize, centerY + halfSize),
            };
            drawList->AddConvexPolyFilled(rightPoly, 4, rightColor);

            // Outline
            drawList->AddRect(ImVec2(centerX - halfSize, centerY - halfSize),
                              ImVec2(centerX + halfSize, centerY + halfSize), IM_COL32(200, 200, 200, 100));
        }
        else
        {
            // Plane / Cylinder / Custom - draw a simple flat quad with material color
            float halfSize = radius * 0.8f;
            float ambient = 0.2f;
            float diffuse = 0.6f;
            float finalR = std::clamp(albedoR * (ambient + diffuse), 0.0f, 1.0f);
            float finalG = std::clamp(albedoG * (ambient + diffuse), 0.0f, 1.0f);
            float finalB = std::clamp(albedoB * (ambient + diffuse), 0.0f, 1.0f);

            ImU32 planeColor = IM_COL32(static_cast<int>(finalR * 255.0f), static_cast<int>(finalG * 255.0f),
                                        static_cast<int>(finalB * 255.0f), 255);
            drawList->AddRectFilled(ImVec2(centerX - halfSize, centerY - halfSize * 0.5f),
                                    ImVec2(centerX + halfSize, centerY + halfSize * 0.5f), planeColor);
            drawList->AddRect(ImVec2(centerX - halfSize, centerY - halfSize * 0.5f),
                              ImVec2(centerX + halfSize, centerY + halfSize * 0.5f), IM_COL32(200, 200, 200, 100));
        }

        // Advance cursor past the preview region
        ImGui::Dummy(previewRegion);
        ImGui::Spacing();

        // Preview info
        const char* shapeNames[] = {"Sphere", "Cube", "Plane", "Cylinder", "Custom"};
        const char* lightingNames[] = {"Default", "Scene Lights", "IBL Only", "Unlit"};
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Shape: %s | Lighting: %s | Rotation: %s",
                           shapeNames[static_cast<int>(m_previewShape)],
                           lightingNames[static_cast<int>(m_previewLighting)], m_previewRotate ? "On" : "Off");

        ImGui::Spacing();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Rotation Speed", &m_previewRotationSpeed, 0.0f, 180.0f, "%.1f deg/s");
    }

    // ========================================================================
    // Default Materials & Parameters
    // ========================================================================

    void MaterialEditorPanel::LoadDefaultMaterials()
    {
        m_materials.clear();

        // Standard PBR
        {
            MaterialDefinition mat;
            mat.name = "Standard PBR";
            mat.shaderPath = "Shaders/StandardPBR.hlsl";
            mat.filePath = "Engine/Materials/StandardPBR.spkmat";
            mat.isBuiltIn = true;
            mat.isModified = false;
            PopulateDefaultPBRParameters(mat);
            m_materials.push_back(std::move(mat));
        }

        // Unlit
        {
            MaterialDefinition mat;
            mat.name = "Unlit";
            mat.shaderPath = "Shaders/Unlit.hlsl";
            mat.filePath = "Engine/Materials/Unlit.spkmat";
            mat.isBuiltIn = true;
            mat.isModified = false;

            ShaderParameter color;
            color.name = "baseColor";
            color.displayName = "Base Color";
            color.type = ShaderParamType::Color;
            color.group = "Surface";
            color.floatValues[0] = 1.0f;
            color.floatValues[1] = 1.0f;
            color.floatValues[2] = 1.0f;
            color.floatValues[3] = 1.0f;
            color.tooltip = "Base color of the material";
            mat.parameters.push_back(color);

            ShaderParameter opacity;
            opacity.name = "opacity";
            opacity.displayName = "Opacity";
            opacity.type = ShaderParamType::Float;
            opacity.group = "Surface";
            opacity.floatValues[0] = 1.0f;
            opacity.minValue = 0.0f;
            opacity.maxValue = 1.0f;
            opacity.tooltip = "Overall opacity of the material";
            mat.parameters.push_back(opacity);

            TextureSlot albedoSlot;
            albedoSlot.name = "Albedo";
            albedoSlot.bindSlot = 0;
            mat.textureSlots.push_back(albedoSlot);

            m_materials.push_back(std::move(mat));
        }

        // Transparent
        {
            MaterialDefinition mat;
            mat.name = "Transparent";
            mat.shaderPath = "Shaders/Transparent.hlsl";
            mat.filePath = "Engine/Materials/Transparent.spkmat";
            mat.isBuiltIn = true;
            mat.isModified = false;
            mat.renderState.blendMode = MaterialRenderState::BlendMode::AlphaBlend;
            mat.renderState.depthWrite = false;
            mat.renderState.renderQueue = 3000;

            PopulateDefaultPBRParameters(mat);

            // Override opacity default
            ShaderParameter* opacityParam = mat.FindParameter("opacity");
            if (opacityParam != nullptr)
            {
                opacityParam->floatValues[0] = 0.5f;
            }

            m_materials.push_back(std::move(mat));
        }

        // Emissive
        {
            MaterialDefinition mat;
            mat.name = "Emissive";
            mat.shaderPath = "Shaders/Emissive.hlsl";
            mat.filePath = "Engine/Materials/Emissive.spkmat";
            mat.isBuiltIn = true;
            mat.isModified = false;

            PopulateDefaultPBRParameters(mat);

            // Set emission defaults
            ShaderParameter* emissionColor = mat.FindParameter("emissionColor");
            if (emissionColor != nullptr)
            {
                emissionColor->floatValues[0] = 1.0f;
                emissionColor->floatValues[1] = 0.8f;
                emissionColor->floatValues[2] = 0.3f;
                emissionColor->floatValues[3] = 1.0f;
            }
            ShaderParameter* emissionIntensity = mat.FindParameter("emissionIntensity");
            if (emissionIntensity != nullptr)
            {
                emissionIntensity->floatValues[0] = 5.0f;
            }

            m_materials.push_back(std::move(mat));
        }

        // Toon
        {
            MaterialDefinition mat;
            mat.name = "Toon";
            mat.shaderPath = "Shaders/Toon.hlsl";
            mat.filePath = "Engine/Materials/Toon.spkmat";
            mat.isBuiltIn = true;
            mat.isModified = false;

            ShaderParameter color;
            color.name = "albedoColor";
            color.displayName = "Albedo Color";
            color.type = ShaderParamType::Color;
            color.group = "Surface";
            color.floatValues[0] = 0.9f;
            color.floatValues[1] = 0.3f;
            color.floatValues[2] = 0.3f;
            color.floatValues[3] = 1.0f;
            color.tooltip = "Main color of the toon material";
            mat.parameters.push_back(color);

            ShaderParameter steps;
            steps.name = "shadingSteps";
            steps.displayName = "Shading Steps";
            steps.type = ShaderParamType::Int;
            steps.group = "Toon";
            steps.intValue = 3;
            steps.minValue = 1.0f;
            steps.maxValue = 8.0f;
            steps.tooltip = "Number of discrete shading levels";
            mat.parameters.push_back(steps);

            ShaderParameter outlineWidth;
            outlineWidth.name = "outlineWidth";
            outlineWidth.displayName = "Outline Width";
            outlineWidth.type = ShaderParamType::Float;
            outlineWidth.group = "Toon";
            outlineWidth.floatValues[0] = 0.02f;
            outlineWidth.minValue = 0.0f;
            outlineWidth.maxValue = 0.1f;
            outlineWidth.step = 0.001f;
            outlineWidth.tooltip = "Width of the cel-shading outline";
            mat.parameters.push_back(outlineWidth);

            ShaderParameter outlineColor;
            outlineColor.name = "outlineColor";
            outlineColor.displayName = "Outline Color";
            outlineColor.type = ShaderParamType::Color;
            outlineColor.group = "Toon";
            outlineColor.floatValues[0] = 0.0f;
            outlineColor.floatValues[1] = 0.0f;
            outlineColor.floatValues[2] = 0.0f;
            outlineColor.floatValues[3] = 1.0f;
            outlineColor.tooltip = "Color of the cel-shading outline";
            mat.parameters.push_back(outlineColor);

            TextureSlot albedoSlot;
            albedoSlot.name = "Albedo";
            albedoSlot.bindSlot = 0;
            mat.textureSlots.push_back(albedoSlot);

            TextureSlot rampSlot;
            rampSlot.name = "Shading Ramp";
            rampSlot.bindSlot = 1;
            rampSlot.wrapU = TextureSlot::WrapMode::Clamp;
            rampSlot.wrapV = TextureSlot::WrapMode::Clamp;
            mat.textureSlots.push_back(rampSlot);

            m_materials.push_back(std::move(mat));
        }

        // Water
        {
            MaterialDefinition mat;
            mat.name = "Water";
            mat.shaderPath = "Shaders/Water.hlsl";
            mat.filePath = "Engine/Materials/Water.spkmat";
            mat.isBuiltIn = true;
            mat.isModified = false;
            mat.renderState.blendMode = MaterialRenderState::BlendMode::AlphaBlend;
            mat.renderState.cullMode = MaterialRenderState::CullMode::None;
            mat.renderState.depthWrite = false;
            mat.renderState.renderQueue = 3000;

            ShaderParameter waterColor;
            waterColor.name = "albedoColor";
            waterColor.displayName = "Water Color";
            waterColor.type = ShaderParamType::Color;
            waterColor.group = "Surface";
            waterColor.floatValues[0] = 0.1f;
            waterColor.floatValues[1] = 0.4f;
            waterColor.floatValues[2] = 0.6f;
            waterColor.floatValues[3] = 0.7f;
            waterColor.tooltip = "Base color and opacity of the water surface";
            mat.parameters.push_back(waterColor);

            ShaderParameter waveSpeed;
            waveSpeed.name = "waveSpeed";
            waveSpeed.displayName = "Wave Speed";
            waveSpeed.type = ShaderParamType::Float;
            waveSpeed.group = "Animation";
            waveSpeed.floatValues[0] = 1.0f;
            waveSpeed.minValue = 0.0f;
            waveSpeed.maxValue = 10.0f;
            waveSpeed.tooltip = "Speed of wave animation";
            mat.parameters.push_back(waveSpeed);

            ShaderParameter waveHeight;
            waveHeight.name = "waveHeight";
            waveHeight.displayName = "Wave Height";
            waveHeight.type = ShaderParamType::Float;
            waveHeight.group = "Animation";
            waveHeight.floatValues[0] = 0.2f;
            waveHeight.minValue = 0.0f;
            waveHeight.maxValue = 5.0f;
            waveHeight.tooltip = "Maximum wave displacement height";
            mat.parameters.push_back(waveHeight);

            ShaderParameter fresnel;
            fresnel.name = "fresnelPower";
            fresnel.displayName = "Fresnel Power";
            fresnel.type = ShaderParamType::Float;
            fresnel.group = "Surface";
            fresnel.floatValues[0] = 3.0f;
            fresnel.minValue = 0.0f;
            fresnel.maxValue = 10.0f;
            fresnel.tooltip = "Controls edge transparency falloff";
            mat.parameters.push_back(fresnel);

            TextureSlot normalSlot;
            normalSlot.name = "Normal Map";
            normalSlot.bindSlot = 0;
            mat.textureSlots.push_back(normalSlot);

            TextureSlot foamSlot;
            foamSlot.name = "Foam";
            foamSlot.bindSlot = 1;
            mat.textureSlots.push_back(foamSlot);

            m_materials.push_back(std::move(mat));
        }

        std::cout << "Material Editor: loaded " << m_materials.size() << " default materials\n";
    }

    void MaterialEditorPanel::PopulateDefaultPBRParameters(MaterialDefinition& material)
    {
        // ---- Surface group ----
        {
            ShaderParameter albedoColor;
            albedoColor.name = "albedoColor";
            albedoColor.displayName = "Albedo Color";
            albedoColor.type = ShaderParamType::Color;
            albedoColor.group = "Surface";
            albedoColor.floatValues[0] = 0.8f;
            albedoColor.floatValues[1] = 0.8f;
            albedoColor.floatValues[2] = 0.8f;
            albedoColor.floatValues[3] = 1.0f;
            albedoColor.tooltip = "Base color of the surface (linear space)";
            material.parameters.push_back(albedoColor);
        }
        {
            ShaderParameter metallic;
            metallic.name = "metallic";
            metallic.displayName = "Metallic";
            metallic.type = ShaderParamType::Float;
            metallic.group = "Surface";
            metallic.floatValues[0] = 0.0f;
            metallic.minValue = 0.0f;
            metallic.maxValue = 1.0f;
            metallic.step = 0.01f;
            metallic.tooltip = "0 = dielectric, 1 = metallic";
            material.parameters.push_back(metallic);
        }
        {
            ShaderParameter roughness;
            roughness.name = "roughness";
            roughness.displayName = "Roughness";
            roughness.type = ShaderParamType::Float;
            roughness.group = "Surface";
            roughness.floatValues[0] = 0.5f;
            roughness.minValue = 0.0f;
            roughness.maxValue = 1.0f;
            roughness.step = 0.01f;
            roughness.tooltip = "0 = mirror-smooth, 1 = fully rough";
            material.parameters.push_back(roughness);
        }
        {
            ShaderParameter opacity;
            opacity.name = "opacity";
            opacity.displayName = "Opacity";
            opacity.type = ShaderParamType::Float;
            opacity.group = "Surface";
            opacity.floatValues[0] = 1.0f;
            opacity.minValue = 0.0f;
            opacity.maxValue = 1.0f;
            opacity.step = 0.01f;
            opacity.tooltip = "Overall opacity of the material";
            material.parameters.push_back(opacity);
        }

        // ---- Normal group ----
        {
            ShaderParameter normalStrength;
            normalStrength.name = "normalStrength";
            normalStrength.displayName = "Normal Strength";
            normalStrength.type = ShaderParamType::Float;
            normalStrength.group = "Normal";
            normalStrength.floatValues[0] = 1.0f;
            normalStrength.minValue = 0.0f;
            normalStrength.maxValue = 2.0f;
            normalStrength.step = 0.01f;
            normalStrength.tooltip = "Intensity of the normal map effect";
            material.parameters.push_back(normalStrength);
        }

        // ---- Emission group ----
        {
            ShaderParameter emissionColor;
            emissionColor.name = "emissionColor";
            emissionColor.displayName = "Emission Color";
            emissionColor.type = ShaderParamType::Color;
            emissionColor.group = "Emission";
            emissionColor.floatValues[0] = 0.0f;
            emissionColor.floatValues[1] = 0.0f;
            emissionColor.floatValues[2] = 0.0f;
            emissionColor.floatValues[3] = 1.0f;
            emissionColor.isHDR = true;
            emissionColor.tooltip = "Emissive color (HDR values allowed)";
            material.parameters.push_back(emissionColor);
        }
        {
            ShaderParameter emissionIntensity;
            emissionIntensity.name = "emissionIntensity";
            emissionIntensity.displayName = "Emission Intensity";
            emissionIntensity.type = ShaderParamType::Float;
            emissionIntensity.group = "Emission";
            emissionIntensity.floatValues[0] = 0.0f;
            emissionIntensity.minValue = 0.0f;
            emissionIntensity.maxValue = 20.0f;
            emissionIntensity.step = 0.1f;
            emissionIntensity.tooltip = "Brightness multiplier for emission";
            material.parameters.push_back(emissionIntensity);
        }

        // ---- Ambient Occlusion group ----
        {
            ShaderParameter aoStrength;
            aoStrength.name = "aoStrength";
            aoStrength.displayName = "AO Strength";
            aoStrength.type = ShaderParamType::Float;
            aoStrength.group = "Ambient Occlusion";
            aoStrength.floatValues[0] = 1.0f;
            aoStrength.minValue = 0.0f;
            aoStrength.maxValue = 1.0f;
            aoStrength.step = 0.01f;
            aoStrength.tooltip = "Strength of the ambient occlusion effect";
            material.parameters.push_back(aoStrength);
        }

        // ---- Detail group ----
        {
            ShaderParameter detailTiling;
            detailTiling.name = "detailTiling";
            detailTiling.displayName = "Detail Tiling";
            detailTiling.type = ShaderParamType::Float2;
            detailTiling.group = "Detail";
            detailTiling.floatValues[0] = 1.0f;
            detailTiling.floatValues[1] = 1.0f;
            detailTiling.minValue = 0.01f;
            detailTiling.maxValue = 50.0f;
            detailTiling.tooltip = "UV tiling for detail textures";
            material.parameters.push_back(detailTiling);
        }
        {
            ShaderParameter useDetailMap;
            useDetailMap.name = "useDetailMap";
            useDetailMap.displayName = "Use Detail Map";
            useDetailMap.type = ShaderParamType::Bool;
            useDetailMap.group = "Detail";
            useDetailMap.boolValue = false;
            useDetailMap.tooltip = "Enable detail texture overlay";
            material.parameters.push_back(useDetailMap);
        }

        // ---- Texture Slots ----
        {
            TextureSlot albedo;
            albedo.name = "Albedo";
            albedo.bindSlot = 0;
            albedo.filter = TextureSlot::FilterMode::Trilinear;
            material.textureSlots.push_back(albedo);
        }
        {
            TextureSlot normal;
            normal.name = "Normal";
            normal.bindSlot = 1;
            normal.filter = TextureSlot::FilterMode::Trilinear;
            material.textureSlots.push_back(normal);
        }
        {
            TextureSlot metallic;
            metallic.name = "Metallic";
            metallic.bindSlot = 2;
            metallic.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(metallic);
        }
        {
            TextureSlot roughness;
            roughness.name = "Roughness";
            roughness.bindSlot = 3;
            roughness.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(roughness);
        }
        {
            TextureSlot ao;
            ao.name = "Ambient Occlusion";
            ao.bindSlot = 4;
            ao.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(ao);
        }
        {
            TextureSlot emission;
            emission.name = "Emission";
            emission.bindSlot = 5;
            emission.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(emission);
        }
        {
            TextureSlot height;
            height.name = "Height";
            height.bindSlot = 6;
            height.filter = TextureSlot::FilterMode::Bilinear;
            material.textureSlots.push_back(height);
        }
        {
            TextureSlot detail;
            detail.name = "Detail";
            detail.bindSlot = 7;
            detail.filter = TextureSlot::FilterMode::Trilinear;
            material.textureSlots.push_back(detail);
        }
    }

} // namespace SparkEditor
