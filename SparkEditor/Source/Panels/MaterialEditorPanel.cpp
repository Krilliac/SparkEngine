/**
 * @file MaterialEditorPanel.cpp
 * @brief Implementation of the visual material editor panel
 * @author Spark Engine Team
 * @date 2026
 */

#include "MaterialEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace SparkEditor
{

    MaterialEditorPanel::MaterialEditorPanel() : EditorPanel("Material Editor", "material_editor_panel") {}

    bool MaterialEditorPanel::Initialize()
    {
        std::cout << "Initializing Material Editor panel\n";

        // Populate available shaders
        m_availableShaders = {
            {"Standard PBR", "Shaders/StandardPBR.hlsl", "Physically-based rendering with metallic workflow"},
            {"Standard Specular", "Shaders/StandardSpecular.hlsl", "PBR with specular workflow"},
            {"Unlit", "Shaders/Unlit.hlsl", "No lighting, texture color only"},
            {"Transparent", "Shaders/Transparent.hlsl", "Alpha-blended transparent surface"},
            {"Emissive", "Shaders/Emissive.hlsl", "Self-illuminating surface with bloom support"},
            {"Terrain", "Shaders/Terrain.hlsl", "Multi-layer terrain blending shader"},
            {"Water", "Shaders/Water.hlsl", "Animated water surface with reflections"},
            {"Toon", "Shaders/Toon.hlsl", "Cel-shaded cartoon style rendering"},
        };

        LoadDefaultMaterials();
        return true;
    }

    void MaterialEditorPanel::Update(float deltaTime)
    {
        if (m_previewRotate)
        {
            m_previewTime += deltaTime;
        }
    }

    void MaterialEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderToolbar();
            ImGui::Separator();

            // Layout: material list on left, properties on right
            float listWidth = 180.0f;
            ImGui::BeginChild("MaterialList", ImVec2(listWidth, 0), true);
            RenderMaterialList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("MaterialDetails", ImVec2(0, 0), true);
            MaterialDefinition* mat = GetSelectedMaterial();
            if (mat)
            {
                RenderShaderSelector();
                ImGui::Separator();
                RenderParameterEditor();
                ImGui::Separator();
                RenderTextureSlots();

                if (m_showRenderState)
                {
                    ImGui::Separator();
                    RenderRenderStateEditor();
                }

                if (m_showPreview)
                {
                    ImGui::Separator();
                    RenderPreview();
                }
            }
            else
            {
                float avail = ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.4f);
                float textW = ImGui::CalcTextSize(ICON_FA_PALETTE " Select a material to edit").x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textW) * 0.5f);
                ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, 1.0f), ICON_FA_PALETTE " Select a material to edit");
            }
            ImGui::EndChild();

            EndPanel();
        }
    }

    void MaterialEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Material Editor panel\n";
        m_materials.clear();
    }

    bool MaterialEditorPanel::HandleEvent(const std::string& /*eventType*/, void* /*eventData*/)
    {
        return false;
    }

    void MaterialEditorPanel::OpenMaterial(const std::string& materialPath)
    {
        // Check if already loaded
        for (int i = 0; i < static_cast<int>(m_materials.size()); ++i)
        {
            if (m_materials[i].filePath == materialPath)
            {
                m_selectedMaterialIndex = i;
                return;
            }
        }

        // Create a new material entry from the path
        MaterialDefinition mat;
        mat.filePath = materialPath;
        // Extract name from path
        size_t lastSlash = materialPath.find_last_of("/\\");
        size_t lastDot = materialPath.find_last_of('.');
        mat.name = materialPath.substr(lastSlash != std::string::npos ? lastSlash + 1 : 0,
                                       lastDot != std::string::npos
                                           ? lastDot - (lastSlash != std::string::npos ? lastSlash + 1 : 0)
                                           : std::string::npos);
        mat.shaderPath = "Shaders/StandardPBR.hlsl";
        PopulateDefaultPBRParameters(mat);
        m_materials.push_back(std::move(mat));
        m_selectedMaterialIndex = static_cast<int>(m_materials.size()) - 1;
    }

    void MaterialEditorPanel::CreateMaterial(const std::string& name, const std::string& shaderPath)
    {
        MaterialDefinition mat;
        mat.name = name;
        mat.shaderPath = shaderPath;
        mat.isModified = true;
        PopulateDefaultPBRParameters(mat);
        m_materials.push_back(std::move(mat));
        m_selectedMaterialIndex = static_cast<int>(m_materials.size()) - 1;
        std::cout << "Created new material: " << name << "\n";
    }

    bool MaterialEditorPanel::SaveMaterial()
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        if (!mat)
            return false;

        mat->isModified = false;
        std::cout << "Saved material: " << mat->name << "\n";
        return true;
    }

    bool MaterialEditorPanel::HasUnsavedChanges() const
    {
        for (const auto& mat : m_materials)
        {
            if (mat.isModified)
                return true;
        }
        return false;
    }

    MaterialDefinition* MaterialEditorPanel::GetSelectedMaterial()
    {
        if (m_selectedMaterialIndex >= 0 && m_selectedMaterialIndex < static_cast<int>(m_materials.size()))
        {
            return &m_materials[m_selectedMaterialIndex];
        }
        return nullptr;
    }

    void MaterialEditorPanel::RenderToolbar()
    {
        if (ImGui::Button(ICON_FA_PLUS " New"))
        {
            int count = static_cast<int>(m_materials.size()) + 1;
            CreateMaterial("New Material " + std::to_string(count), "Shaders/StandardPBR.hlsl");
        }
        ImGui::SameLine();

        bool hasMat = (GetSelectedMaterial() != nullptr);
        if (!hasMat)
            ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            SaveMaterial();
        }
        if (!hasMat)
            ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::Checkbox("Preview", &m_showPreview);
        ImGui::SameLine();
        ImGui::Checkbox("Render State", &m_showRenderState);
    }

    void MaterialEditorPanel::RenderMaterialList()
    {
        ImGui::Text(ICON_FA_PALETTE " Materials");
        ImGui::Separator();

        // Search filter
        char filterBuf[128];
        std::strncpy(filterBuf, m_searchFilter.c_str(), sizeof(filterBuf) - 1);
        filterBuf[sizeof(filterBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##MatSearch", ICON_FA_SEARCH " Search...", filterBuf, sizeof(filterBuf)))
        {
            m_searchFilter = filterBuf;
        }

        ImGui::Spacing();

        for (int i = 0; i < static_cast<int>(m_materials.size()); ++i)
        {
            const auto& mat = m_materials[i];

            // Apply search filter
            if (!m_searchFilter.empty())
            {
                std::string lower = mat.name;
                std::string filter = m_searchFilter;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(filter.begin(), filter.end(), filter.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find(filter) == std::string::npos)
                    continue;
            }

            char label[128];
            std::snprintf(label, sizeof(label), "%s%s##mat%d", mat.isModified ? "* " : "", mat.name.c_str(), i);
            if (ImGui::Selectable(label, m_selectedMaterialIndex == i))
            {
                m_selectedMaterialIndex = i;
            }
        }
    }

    void MaterialEditorPanel::RenderShaderSelector()
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        if (!mat)
            return;

        ImGui::Text(ICON_FA_CODE " Shader");

        // Find current shader name
        std::string currentShaderName = mat->shaderPath;
        for (const auto& shader : m_availableShaders)
        {
            if (shader.path == mat->shaderPath)
            {
                currentShaderName = shader.name;
                break;
            }
        }

        if (ImGui::BeginCombo("##ShaderCombo", currentShaderName.c_str()))
        {
            for (const auto& shader : m_availableShaders)
            {
                bool isSelected = (mat->shaderPath == shader.path);
                if (ImGui::Selectable(shader.name.c_str(), isSelected))
                {
                    mat->shaderPath = shader.path;
                    mat->isModified = true;
                }
                if (ImGui::IsItemHovered() && !shader.description.empty())
                {
                    ImGui::SetTooltip("%s", shader.description.c_str());
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    void MaterialEditorPanel::RenderParameterEditor()
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        if (!mat)
            return;

        ImGui::Text(ICON_FA_SLIDERS_H " Properties");
        ImGui::Separator();

        // Group parameters
        std::unordered_map<std::string, std::vector<ShaderParameter*>> groups;
        for (auto& param : mat->parameters)
        {
            groups[param.group.empty() ? "General" : param.group].push_back(&param);
        }

        for (auto& [groupName, params] : groups)
        {
            RenderParameterGroup(groupName, params);
        }
    }

    void MaterialEditorPanel::RenderParameterGroup(const std::string& groupName, std::vector<ShaderParameter*>& params)
    {
        if (ImGui::TreeNodeEx(groupName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (auto* param : params)
            {
                switch (param->type)
                {
                case ShaderParamType::Float:
                    RenderFloatParam(*param);
                    break;
                case ShaderParamType::Float2:
                case ShaderParamType::Float3:
                case ShaderParamType::Float4:
                    RenderVectorParam(*param);
                    break;
                case ShaderParamType::Color:
                    RenderColorParam(*param);
                    break;
                case ShaderParamType::Bool:
                    RenderBoolParam(*param);
                    break;
                case ShaderParamType::Texture2D:
                case ShaderParamType::TextureCube:
                    RenderTextureParam(*param);
                    break;
                case ShaderParamType::Int:
                {
                    MaterialDefinition* mat = GetSelectedMaterial();
                    std::string label = "##" + param->name;
                    ImGui::Text("%s", param->displayName.c_str());
                    ImGui::SameLine(130);
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::DragInt(label.c_str(), &param->intValue))
                    {
                        if (mat)
                            mat->isModified = true;
                    }
                    break;
                }
                case ShaderParamType::Matrix4x4:
                    ImGui::Text("%s (Matrix4x4)", param->displayName.c_str());
                    break;
                }
            }
            ImGui::TreePop();
        }
    }

    void MaterialEditorPanel::RenderFloatParam(ShaderParameter& param)
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        std::string label = "##" + param.name;
        ImGui::Text("%s", param.displayName.c_str());
        if (!param.tooltip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", param.tooltip.c_str());
        ImGui::SameLine(130);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat(label.c_str(), &param.floatValues[0], param.minValue, param.maxValue))
        {
            if (mat)
                mat->isModified = true;
        }
    }

    void MaterialEditorPanel::RenderVectorParam(ShaderParameter& param)
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        std::string label = "##" + param.name;
        ImGui::Text("%s", param.displayName.c_str());
        ImGui::SameLine(130);
        ImGui::SetNextItemWidth(-1);

        int components = (param.type == ShaderParamType::Float2) ? 2 : (param.type == ShaderParamType::Float3) ? 3 : 4;
        bool changed = false;
        if (components == 2)
            changed = ImGui::DragFloat2(label.c_str(), param.floatValues, param.step);
        else if (components == 3)
            changed = ImGui::DragFloat3(label.c_str(), param.floatValues, param.step);
        else
            changed = ImGui::DragFloat4(label.c_str(), param.floatValues, param.step);

        if (changed && mat)
            mat->isModified = true;
    }

    void MaterialEditorPanel::RenderColorParam(ShaderParameter& param)
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        std::string label = "##" + param.name;
        ImGui::Text("%s", param.displayName.c_str());
        ImGui::SameLine(130);
        ImGui::SetNextItemWidth(-1);
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar;
        if (param.isHDR)
            flags |= ImGuiColorEditFlags_Float;
        if (ImGui::ColorEdit4(label.c_str(), param.floatValues, flags))
        {
            if (mat)
                mat->isModified = true;
        }
    }

    void MaterialEditorPanel::RenderBoolParam(ShaderParameter& param)
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        std::string label = "##" + param.name;
        if (ImGui::Checkbox((param.displayName + label).c_str(), &param.boolValue))
        {
            if (mat)
                mat->isModified = true;
        }
    }

    void MaterialEditorPanel::RenderTextureParam(ShaderParameter& param)
    {
        ImGui::Text("%s", param.displayName.c_str());
        ImGui::SameLine(130);

        if (param.texturePath.empty())
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(None)");
        }
        else
        {
            ImGui::Text("%s", param.texturePath.c_str());
        }

        ImGui::SameLine();
        std::string btnLabel = ICON_FA_FOLDER_OPEN "##tex_" + param.name;
        if (ImGui::SmallButton(btnLabel.c_str()))
        {
            // Texture browser would open here
        }
    }

    void MaterialEditorPanel::RenderTextureSlots()
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        if (!mat)
            return;

        if (ImGui::CollapsingHeader(ICON_FA_IMAGE " Texture Slots", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (auto& slot : mat->textureSlots)
            {
                RenderTextureSlotEditor(slot);
                ImGui::Spacing();
            }
        }
    }

    void MaterialEditorPanel::RenderTextureSlotEditor(TextureSlot& slot)
    {
        MaterialDefinition* mat = GetSelectedMaterial();

        ImGui::PushID(slot.name.c_str());

        // Texture slot header
        ImGui::Text("%s (Slot %d)", slot.name.c_str(), slot.bindSlot);
        ImGui::SameLine(200);

        if (slot.isAssigned)
        {
            ImGui::Text("%s", slot.texturePath.c_str());
        }
        else
        {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(Empty)");
        }

        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN "##browse"))
        {
            // Texture browser would open here
        }

        if (slot.isAssigned && ImGui::TreeNode("Settings"))
        {
            // Filter mode
            const char* filterNames[] = {"Point", "Bilinear", "Trilinear", "Anisotropic"};
            int filterIdx = static_cast<int>(slot.filter);
            if (ImGui::Combo("Filter", &filterIdx, filterNames, 4))
            {
                slot.filter = static_cast<TextureSlot::FilterMode>(filterIdx);
                if (mat)
                    mat->isModified = true;
            }

            // Wrap mode
            const char* wrapNames[] = {"Repeat", "Clamp", "Mirror", "Border"};
            int wrapUIdx = static_cast<int>(slot.wrapU);
            int wrapVIdx = static_cast<int>(slot.wrapV);
            if (ImGui::Combo("Wrap U", &wrapUIdx, wrapNames, 4))
            {
                slot.wrapU = static_cast<TextureSlot::WrapMode>(wrapUIdx);
                if (mat)
                    mat->isModified = true;
            }
            if (ImGui::Combo("Wrap V", &wrapVIdx, wrapNames, 4))
            {
                slot.wrapV = static_cast<TextureSlot::WrapMode>(wrapVIdx);
                if (mat)
                    mat->isModified = true;
            }

            // Tiling and offset
            float tiling[2] = {slot.tilingU, slot.tilingV};
            if (ImGui::DragFloat2("Tiling", tiling, 0.01f, 0.01f, 100.0f))
            {
                slot.tilingU = tiling[0];
                slot.tilingV = tiling[1];
                if (mat)
                    mat->isModified = true;
            }

            float offset[2] = {slot.offsetU, slot.offsetV};
            if (ImGui::DragFloat2("Offset", offset, 0.01f, -10.0f, 10.0f))
            {
                slot.offsetU = offset[0];
                slot.offsetV = offset[1];
                if (mat)
                    mat->isModified = true;
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void MaterialEditorPanel::RenderRenderStateEditor()
    {
        MaterialDefinition* mat = GetSelectedMaterial();
        if (!mat)
            return;

        if (ImGui::CollapsingHeader(ICON_FA_COG " Render State"))
        {
            auto& rs = mat->renderState;

            // Blend mode
            const char* blendNames[] = {"Opaque", "Alpha Blend", "Additive", "Multiply", "Custom"};
            int blendIdx = static_cast<int>(rs.blendMode);
            if (ImGui::Combo("Blend Mode", &blendIdx, blendNames, 5))
            {
                rs.blendMode = static_cast<MaterialRenderState::BlendMode>(blendIdx);
                mat->isModified = true;
            }

            // Cull mode
            const char* cullNames[] = {"Back", "Front", "None"};
            int cullIdx = static_cast<int>(rs.cullMode);
            if (ImGui::Combo("Cull Mode", &cullIdx, cullNames, 3))
            {
                rs.cullMode = static_cast<MaterialRenderState::CullMode>(cullIdx);
                mat->isModified = true;
            }

            if (ImGui::Checkbox("Depth Write", &rs.depthWrite))
                mat->isModified = true;
            if (ImGui::Checkbox("Depth Test", &rs.depthTest))
                mat->isModified = true;
            if (ImGui::Checkbox("Cast Shadows", &rs.castShadows))
                mat->isModified = true;
            if (ImGui::Checkbox("Receive Shadows", &rs.receiveShadows))
                mat->isModified = true;

            if (rs.blendMode == MaterialRenderState::BlendMode::AlphaBlend)
            {
                if (ImGui::SliderFloat("Alpha Clip", &rs.alphaClipThreshold, 0.0f, 1.0f))
                    mat->isModified = true;
            }

            if (ImGui::DragInt("Render Queue", &rs.renderQueue, 1.0f, 0, 5000))
                mat->isModified = true;
        }
    }

    void MaterialEditorPanel::RenderPreview()
    {
        if (ImGui::CollapsingHeader(ICON_FA_EYE " Preview", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Preview shape selector
            const char* shapeNames[] = {"Sphere", "Cube", "Plane", "Cylinder", "Custom"};
            int shapeIdx = static_cast<int>(m_previewShape);
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##Shape", &shapeIdx, shapeNames, 5))
            {
                m_previewShape = static_cast<PreviewShape>(shapeIdx);
            }
            ImGui::SameLine();

            const char* lightNames[] = {"Default", "Scene Lights", "IBL Only", "Unlit"};
            int lightIdx = static_cast<int>(m_previewLighting);
            ImGui::SetNextItemWidth(100);
            if (ImGui::Combo("##Lighting", &lightIdx, lightNames, 4))
            {
                m_previewLighting = static_cast<PreviewLighting>(lightIdx);
            }
            ImGui::SameLine();
            ImGui::Checkbox("Rotate", &m_previewRotate);

            // Preview area
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float previewH = std::min(180.0f, avail.y);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size(avail.x, previewH);

            // Background
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(30, 32, 38, 255), 4.0f);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(60, 70, 80, 200), 4.0f);

            // Draw preview shape visualization
            MaterialDefinition* mat = GetSelectedMaterial();
            float cx = pos.x + size.x * 0.5f;
            float cy = pos.y + size.y * 0.5f;
            float radius = std::min(size.x, size.y) * 0.3f;

            // Get material color for the preview
            float r = 0.7f;
            float g = 0.7f;
            float b = 0.7f;
            if (mat)
            {
                auto* albedo = mat->FindParameter("albedoColor");
                if (albedo && albedo->type == ShaderParamType::Color)
                {
                    r = albedo->floatValues[0];
                    g = albedo->floatValues[1];
                    b = albedo->floatValues[2];
                }
            }

            // Simulate rotation
            float angle = m_previewTime * (m_previewRotationSpeed * 0.0174533f);
            float lightX = std::cos(angle) * 0.5f + 0.5f;

            // Draw sphere-like gradient
            int segments = 32;
            for (int i = 0; i < segments; ++i)
            {
                float t = static_cast<float>(i) / segments;
                float shade = 0.3f + 0.7f * std::max(0.0f, 1.0f - std::abs(t - lightX) * 2.0f);
                ImU32 col = IM_COL32(static_cast<int>(r * shade * 255), static_cast<int>(g * shade * 255),
                                     static_cast<int>(b * shade * 255), 255);
                float segRadius = radius * std::sin(t * 3.14159f);
                float segX = cx - radius + t * radius * 2.0f;
                dl->AddLine(ImVec2(segX, cy - segRadius), ImVec2(segX, cy + segRadius), col, 3.0f);
            }

            ImGui::Dummy(ImVec2(size.x, previewH));
        }
    }

    void MaterialEditorPanel::LoadDefaultMaterials()
    {
        // Default PBR material
        {
            MaterialDefinition mat;
            mat.name = "Default";
            mat.shaderPath = "Shaders/StandardPBR.hlsl";
            mat.isBuiltIn = true;
            PopulateDefaultPBRParameters(mat);
            m_materials.push_back(std::move(mat));
        }

        // Ground material
        {
            MaterialDefinition mat;
            mat.name = "Ground";
            mat.shaderPath = "Shaders/StandardPBR.hlsl";
            PopulateDefaultPBRParameters(mat);
            if (auto* albedo = mat.FindParameter("albedoColor"))
            {
                albedo->floatValues[0] = 0.4f;
                albedo->floatValues[1] = 0.35f;
                albedo->floatValues[2] = 0.25f;
                albedo->floatValues[3] = 1.0f;
            }
            if (auto* roughness = mat.FindParameter("roughness"))
            {
                roughness->floatValues[0] = 0.85f;
            }
            m_materials.push_back(std::move(mat));
        }

        // Metal material
        {
            MaterialDefinition mat;
            mat.name = "Metal";
            mat.shaderPath = "Shaders/StandardPBR.hlsl";
            PopulateDefaultPBRParameters(mat);
            if (auto* albedo = mat.FindParameter("albedoColor"))
            {
                albedo->floatValues[0] = 0.9f;
                albedo->floatValues[1] = 0.9f;
                albedo->floatValues[2] = 0.9f;
                albedo->floatValues[3] = 1.0f;
            }
            if (auto* metallic = mat.FindParameter("metallic"))
            {
                metallic->floatValues[0] = 1.0f;
            }
            if (auto* roughness = mat.FindParameter("roughness"))
            {
                roughness->floatValues[0] = 0.2f;
            }
            m_materials.push_back(std::move(mat));
        }

        // Emissive material
        {
            MaterialDefinition mat;
            mat.name = "Emissive Glow";
            mat.shaderPath = "Shaders/Emissive.hlsl";
            PopulateDefaultPBRParameters(mat);
            if (auto* emission = mat.FindParameter("emissionColor"))
            {
                emission->floatValues[0] = 1.0f;
                emission->floatValues[1] = 0.4f;
                emission->floatValues[2] = 0.1f;
                emission->floatValues[3] = 1.0f;
            }
            if (auto* emissionStr = mat.FindParameter("emissionIntensity"))
            {
                emissionStr->floatValues[0] = 5.0f;
            }
            m_materials.push_back(std::move(mat));
        }
    }

    void MaterialEditorPanel::PopulateDefaultPBRParameters(MaterialDefinition& material)
    {
        material.parameters.clear();
        material.textureSlots.clear();

        // Surface group
        ShaderParameter albedo;
        albedo.name = "albedoColor";
        albedo.displayName = "Albedo Color";
        albedo.type = ShaderParamType::Color;
        albedo.group = "Surface";
        albedo.floatValues[0] = 0.8f;
        albedo.floatValues[1] = 0.8f;
        albedo.floatValues[2] = 0.8f;
        albedo.floatValues[3] = 1.0f;
        albedo.tooltip = "Base color of the surface";
        material.parameters.push_back(albedo);

        ShaderParameter metallic;
        metallic.name = "metallic";
        metallic.displayName = "Metallic";
        metallic.type = ShaderParamType::Float;
        metallic.group = "Surface";
        metallic.floatValues[0] = 0.0f;
        metallic.minValue = 0.0f;
        metallic.maxValue = 1.0f;
        metallic.tooltip = "Metallic factor (0 = dielectric, 1 = metal)";
        material.parameters.push_back(metallic);

        ShaderParameter roughness;
        roughness.name = "roughness";
        roughness.displayName = "Roughness";
        roughness.type = ShaderParamType::Float;
        roughness.group = "Surface";
        roughness.floatValues[0] = 0.5f;
        roughness.minValue = 0.0f;
        roughness.maxValue = 1.0f;
        roughness.tooltip = "Surface roughness (0 = smooth, 1 = rough)";
        material.parameters.push_back(roughness);

        // Normal group
        ShaderParameter normalStrength;
        normalStrength.name = "normalStrength";
        normalStrength.displayName = "Normal Strength";
        normalStrength.type = ShaderParamType::Float;
        normalStrength.group = "Normal";
        normalStrength.floatValues[0] = 1.0f;
        normalStrength.minValue = 0.0f;
        normalStrength.maxValue = 2.0f;
        material.parameters.push_back(normalStrength);

        // Emission group
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
        material.parameters.push_back(emissionColor);

        ShaderParameter emissionIntensity;
        emissionIntensity.name = "emissionIntensity";
        emissionIntensity.displayName = "Emission Intensity";
        emissionIntensity.type = ShaderParamType::Float;
        emissionIntensity.group = "Emission";
        emissionIntensity.floatValues[0] = 0.0f;
        emissionIntensity.minValue = 0.0f;
        emissionIntensity.maxValue = 20.0f;
        material.parameters.push_back(emissionIntensity);

        // AO
        ShaderParameter aoStrength;
        aoStrength.name = "aoStrength";
        aoStrength.displayName = "AO Strength";
        aoStrength.type = ShaderParamType::Float;
        aoStrength.group = "Occlusion";
        aoStrength.floatValues[0] = 1.0f;
        aoStrength.minValue = 0.0f;
        aoStrength.maxValue = 1.0f;
        material.parameters.push_back(aoStrength);

        // Texture slots
        TextureSlot albedoTex;
        albedoTex.name = "Albedo";
        albedoTex.bindSlot = 0;
        material.textureSlots.push_back(albedoTex);

        TextureSlot normalTex;
        normalTex.name = "Normal";
        normalTex.bindSlot = 1;
        material.textureSlots.push_back(normalTex);

        TextureSlot metallicTex;
        metallicTex.name = "Metallic";
        metallicTex.bindSlot = 2;
        material.textureSlots.push_back(metallicTex);

        TextureSlot roughnessTex;
        roughnessTex.name = "Roughness";
        roughnessTex.bindSlot = 3;
        material.textureSlots.push_back(roughnessTex);

        TextureSlot aoTex;
        aoTex.name = "Ambient Occlusion";
        aoTex.bindSlot = 4;
        material.textureSlots.push_back(aoTex);

        TextureSlot emissionTex;
        emissionTex.name = "Emission";
        emissionTex.bindSlot = 5;
        material.textureSlots.push_back(emissionTex);
    }

} // namespace SparkEditor
