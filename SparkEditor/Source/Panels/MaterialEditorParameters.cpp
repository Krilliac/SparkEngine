/**
 * @file MaterialEditorParameters.cpp
 * @brief Parameter and texture editing for the material editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains: RenderShaderSelector, RenderParameterEditor, RenderParameterGroup,
 * RenderFloatParam, RenderVectorParam, RenderColorParam, RenderBoolParam,
 * RenderTextureParam, RenderTextureSlots, RenderTextureSlotEditor.
 */

#include "MaterialEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/ContainerUtils.h"
#include <imgui.h>
#include <algorithm>
#include <iostream>

namespace SparkEditor
{

    // ========================================================================
    // Shader Selector
    // ========================================================================

    void MaterialEditorPanel::RenderShaderSelector()
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        if (selected == nullptr)
        {
            return;
        }

        ImGui::Text(ICON_FA_CODE " Shader");
        ImGui::SameLine();

        // Find current shader index
        int currentShaderIndex = -1;
        for (int i = 0; i < static_cast<int>(m_availableShaders.size()); ++i)
        {
            if (m_availableShaders[i].path == selected->shaderPath)
            {
                currentShaderIndex = i;
                break;
            }
        }

        const char* currentName = (currentShaderIndex >= 0) ? m_availableShaders[currentShaderIndex].name.c_str()
                                                            : selected->shaderPath.c_str();

        ImGui::SetNextItemWidth(-1.0f);
        if (selected->isBuiltIn)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::BeginCombo("##ShaderCombo", currentName))
        {
            for (int i = 0; i < static_cast<int>(m_availableShaders.size()); ++i)
            {
                bool isSelected = (i == currentShaderIndex);
                if (ImGui::Selectable(m_availableShaders[i].name.c_str(), isSelected))
                {
                    selected->shaderPath = m_availableShaders[i].path;
                    selected->isModified = true;
                    SetModified(true);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s\n%s", m_availableShaders[i].description.c_str(),
                                      m_availableShaders[i].path.c_str());
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (selected->isBuiltIn)
        {
            ImGui::EndDisabled();
        }
    }

    // ========================================================================
    // Parameter Editor
    // ========================================================================

    void MaterialEditorPanel::RenderParameterEditor()
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        if (selected == nullptr)
        {
            return;
        }

        if (selected->isBuiltIn)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), ICON_FA_LOCK " Built-in material (read-only)");
            ImGui::Spacing();
        }

        // Group parameters by their group name
        std::unordered_map<std::string, std::vector<ShaderParameter*>> groups;
        std::vector<std::string> groupOrder;

        for (auto& param : selected->parameters)
        {
            const std::string& group = param.group.empty() ? "General" : param.group;
            if (!Spark::ContainerUtils::Contains(groups, group))
            {
                groupOrder.push_back(group);
            }
            groups[group].push_back(&param);
        }

        for (const auto& groupName : groupOrder)
        {
            auto& params = groups[groupName];
            RenderParameterGroup(groupName, params);
        }
    }

    void MaterialEditorPanel::RenderParameterGroup(const std::string& groupName, std::vector<ShaderParameter*>& params)
    {
        if (ImGui::CollapsingHeader(groupName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);
            MaterialDefinition* selected = GetSelectedMaterial();
            bool isReadOnly = (selected != nullptr && selected->isBuiltIn);

            if (isReadOnly)
            {
                ImGui::BeginDisabled();
            }

            for (auto* param : params)
            {
                ImGui::PushID(param->name.c_str());
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
                    int minVal = static_cast<int>(param->minValue);
                    int maxVal = static_cast<int>(param->maxValue);
                    if (ImGui::SliderInt(param->displayName.c_str(), &param->intValue, minVal, maxVal))
                    {
                        if (selected != nullptr)
                        {
                            selected->isModified = true;
                            SetModified(true);
                        }
                    }
                    break;
                }
                case ShaderParamType::Matrix4x4:
                    ImGui::Text("%s: [4x4 Matrix]", param->displayName.c_str());
                    break;
                }

                if (!param->tooltip.empty() && ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", param->tooltip.c_str());
                }

                ImGui::PopID();
            }

            if (isReadOnly)
            {
                ImGui::EndDisabled();
            }

            ImGui::Unindent(8.0f);
        }
    }

    void MaterialEditorPanel::RenderFloatParam(ShaderParameter& param)
    {
        MaterialDefinition* selected = GetSelectedMaterial();

        if (ImGui::SliderFloat(param.displayName.c_str(), &param.floatValues[0], param.minValue, param.maxValue,
                               "%.3f"))
        {
            if (selected != nullptr)
            {
                selected->isModified = true;
                SetModified(true);
            }
        }
    }

    void MaterialEditorPanel::RenderVectorParam(ShaderParameter& param)
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        bool changed = false;

        switch (param.type)
        {
        case ShaderParamType::Float2:
            changed = ImGui::SliderFloat2(param.displayName.c_str(), param.floatValues, param.minValue, param.maxValue,
                                          "%.3f");
            break;
        case ShaderParamType::Float3:
            changed = ImGui::SliderFloat3(param.displayName.c_str(), param.floatValues, param.minValue, param.maxValue,
                                          "%.3f");
            break;
        case ShaderParamType::Float4:
            changed = ImGui::SliderFloat4(param.displayName.c_str(), param.floatValues, param.minValue, param.maxValue,
                                          "%.3f");
            break;
        default:
            break;
        }

        if (changed && selected != nullptr)
        {
            selected->isModified = true;
            SetModified(true);
        }
    }

    void MaterialEditorPanel::RenderColorParam(ShaderParameter& param)
    {
        MaterialDefinition* selected = GetSelectedMaterial();

        ImGuiColorEditFlags flags =
            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_PickerHueWheel;
        if (param.isHDR)
        {
            flags |= ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float;
        }

        if (ImGui::ColorEdit4(param.displayName.c_str(), param.floatValues, flags))
        {
            if (selected != nullptr)
            {
                selected->isModified = true;
                SetModified(true);
            }
        }
    }

    void MaterialEditorPanel::RenderBoolParam(ShaderParameter& param)
    {
        MaterialDefinition* selected = GetSelectedMaterial();

        if (ImGui::Checkbox(param.displayName.c_str(), &param.boolValue))
        {
            if (selected != nullptr)
            {
                selected->isModified = true;
                SetModified(true);
            }
        }
    }

    void MaterialEditorPanel::RenderTextureParam(ShaderParameter& param)
    {
        MaterialDefinition* selected = GetSelectedMaterial();

        ImGui::Text("%s", param.displayName.c_str());
        ImGui::SameLine();

        // Texture path input with browse button
        char pathBuf[256] = {};
        std::copy(param.texturePath.begin(),
                  param.texturePath.begin() +
                      std::min(param.texturePath.size(), static_cast<size_t>(sizeof(pathBuf) - 1)),
                  pathBuf);

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
        std::string inputId = "##TexPath_" + param.name;
        if (ImGui::InputText(inputId.c_str(), pathBuf, sizeof(pathBuf)))
        {
            param.texturePath = pathBuf;
            if (selected != nullptr)
            {
                selected->isModified = true;
                SetModified(true);
            }
        }

        ImGui::SameLine();
        std::string browseId = ICON_FA_FOLDER "##Browse_" + param.name;
        if (ImGui::Button(browseId.c_str()))
        {
            // Open asset browser for texture selection (placeholder)
            std::cout << "Material Editor: browse texture for '" << param.name << "'\n";
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Browse for texture asset");
        }
    }

    // ========================================================================
    // Texture Slots
    // ========================================================================

    void MaterialEditorPanel::RenderTextureSlots()
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        if (selected == nullptr)
        {
            return;
        }

        ImGui::Text(ICON_FA_IMAGE " Texture Slots (%d)", static_cast<int>(selected->textureSlots.size()));
        ImGui::Separator();
        ImGui::Spacing();

        if (selected->isBuiltIn)
        {
            ImGui::BeginDisabled();
        }

        for (auto& slot : selected->textureSlots)
        {
            ImGui::PushID(slot.name.c_str());
            RenderTextureSlotEditor(slot);
            ImGui::PopID();
            ImGui::Spacing();
        }

        if (selected->isBuiltIn)
        {
            ImGui::EndDisabled();
        }
    }

    void MaterialEditorPanel::RenderTextureSlotEditor(TextureSlot& slot)
    {
        MaterialDefinition* selected = GetSelectedMaterial();

        // Collapsing header for each slot
        std::string headerLabel = slot.name + " (Slot " + std::to_string(slot.bindSlot) + ")";
        if (slot.isAssigned)
        {
            headerLabel = ICON_FA_CHECK " " + headerLabel;
        }
        else
        {
            headerLabel = ICON_FA_TIMES " " + headerLabel;
        }

        if (ImGui::CollapsingHeader(headerLabel.c_str()))
        {
            ImGui::Indent(8.0f);

            // Texture path
            char pathBuf[256] = {};
            std::copy(slot.texturePath.begin(),
                      slot.texturePath.begin() +
                          std::min(slot.texturePath.size(), static_cast<size_t>(sizeof(pathBuf) - 1)),
                      pathBuf);

            ImGui::Text("Texture:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
            if (ImGui::InputText("##SlotTexture", pathBuf, sizeof(pathBuf)))
            {
                slot.texturePath = pathBuf;
                slot.isAssigned = !slot.texturePath.empty();
                if (selected != nullptr)
                {
                    selected->isModified = true;
                    SetModified(true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER "##SlotBrowse"))
            {
                std::cout << "Material Editor: browse texture for slot '" << slot.name << "'\n";
            }

            // Clear button
            if (slot.isAssigned)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_TIMES "##SlotClear"))
                {
                    slot.texturePath.clear();
                    slot.isAssigned = false;
                    if (selected != nullptr)
                    {
                        selected->isModified = true;
                        SetModified(true);
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Clear texture assignment");
                }
            }

            // Filter mode
            const char* filterNames[] = {"Point", "Bilinear", "Trilinear", "Anisotropic"};
            int currentFilter = static_cast<int>(slot.filter);
            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Filter", &currentFilter, filterNames, 4))
            {
                slot.filter = static_cast<TextureSlot::FilterMode>(currentFilter);
                if (selected != nullptr)
                {
                    selected->isModified = true;
                    SetModified(true);
                }
            }

            // Wrap modes
            const char* wrapNames[] = {"Repeat", "Clamp", "Mirror", "Border"};
            int currentWrapU = static_cast<int>(slot.wrapU);
            int currentWrapV = static_cast<int>(slot.wrapV);

            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Wrap U", &currentWrapU, wrapNames, 4))
            {
                slot.wrapU = static_cast<TextureSlot::WrapMode>(currentWrapU);
                if (selected != nullptr)
                {
                    selected->isModified = true;
                    SetModified(true);
                }
            }

            ImGui::SetNextItemWidth(150.0f);
            if (ImGui::Combo("Wrap V", &currentWrapV, wrapNames, 4))
            {
                slot.wrapV = static_cast<TextureSlot::WrapMode>(currentWrapV);
                if (selected != nullptr)
                {
                    selected->isModified = true;
                    SetModified(true);
                }
            }

            // Tiling and offset
            if (ImGui::DragFloat2("Tiling", &slot.tilingU, 0.01f, 0.01f, 100.0f, "%.2f"))
            {
                if (selected != nullptr)
                {
                    selected->isModified = true;
                    SetModified(true);
                }
            }
            if (ImGui::DragFloat2("Offset", &slot.offsetU, 0.01f, -10.0f, 10.0f, "%.3f"))
            {
                if (selected != nullptr)
                {
                    selected->isModified = true;
                    SetModified(true);
                }
            }

            ImGui::Unindent(8.0f);
        }
    }

} // namespace SparkEditor
