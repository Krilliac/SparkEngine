/**
 * @file MaterialEditorPanel.cpp
 * @brief Visual material and shader property editor implementation
 * @author Spark Engine Team
 * @date 2025
 */

#include "MaterialEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <fstream>

namespace SparkEditor
{

    // ========================================================================
    // Construction / Lifecycle
    // ========================================================================

    MaterialEditorPanel::MaterialEditorPanel() : EditorPanel("Material Editor", "material_editor_panel") {}

    bool MaterialEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        std::cout << "Initializing Material Editor panel\n";

        SetIcon(ICON_FA_PALETTE);

        // Register available shaders
        m_availableShaders = {
            {"Standard PBR", "Shaders/StandardPBR.hlsl", "Physically-based rendering with metallic workflow"},
            {"Standard Specular", "Shaders/StandardSpecular.hlsl", "PBR with specular/glossiness workflow"},
            {"Unlit", "Shaders/Unlit.hlsl", "No lighting calculations, flat color or texture"},
            {"Transparent", "Shaders/Transparent.hlsl", "Alpha-blended transparent surfaces"},
            {"Emissive", "Shaders/Emissive.hlsl", "Self-illuminating materials with bloom support"},
            {"Toon", "Shaders/Toon.hlsl", "Cel-shaded cartoon-style rendering"},
            {"Glass", "Shaders/Glass.hlsl", "Refractive glass with fresnel and tint"},
            {"Terrain", "Shaders/Terrain.hlsl", "Multi-layer splatmap terrain blending"},
            {"Water", "Shaders/Water.hlsl", "Animated water surface with reflections"},
            {"Particle", "Shaders/Particle.hlsl", "Particle system material with soft blending"},
            {"Skybox", "Shaders/Skybox.hlsl", "Cubemap-based sky rendering"},
            {"Decal", "Shaders/Decal.hlsl", "Projected decal material"},
        };

        LoadDefaultMaterials();

        m_isInitialized = true;
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

            // Two-column layout: material list on the left, editor on the right
            float listWidth = 200.0f;
            float availableHeight = ImGui::GetContentRegionAvail().y;

            ImGui::BeginChild("MaterialListRegion", ImVec2(listWidth, availableHeight), true);
            RenderMaterialList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("MaterialEditorRegion", ImVec2(0, availableHeight), true);
            MaterialDefinition* selected = GetSelectedMaterial();
            if (selected != nullptr)
            {
                // Material name header
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f), "%s %s", ICON_FA_PALETTE, selected->name.c_str());
                if (selected->isModified)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "(unsaved)");
                }
                if (selected->isBuiltIn)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "[built-in]");
                }
                ImGui::Separator();

                RenderShaderSelector();
                ImGui::Separator();

                // Tabbed sections for parameters, textures, render state, preview
                if (ImGui::BeginTabBar("MaterialTabs"))
                {
                    if (ImGui::BeginTabItem(ICON_FA_SLIDERS " Parameters"))
                    {
                        RenderParameterEditor();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem(ICON_FA_IMAGE " Textures"))
                    {
                        RenderTextureSlots();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem(ICON_FA_COG " Render State"))
                    {
                        RenderRenderStateEditor();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem(ICON_FA_EYE " Preview"))
                    {
                        RenderPreview();
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                   "Select a material from the list or create a new one.");
            }
            ImGui::EndChild();
        }
        EndPanel();
    }

    void MaterialEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Material Editor panel\n";
        m_materials.clear();
        m_availableShaders.clear();
        m_selectedMaterialIndex = -1;
        m_isInitialized = false;
    }

    bool MaterialEditorPanel::HandleEvent(const std::string& eventType, void* eventData)
    {
        if (eventType == "OpenMaterial" && eventData != nullptr)
        {
            const auto* path = static_cast<const std::string*>(eventData);
            OpenMaterial(*path);
            return true;
        }
        if (eventType == "CreateMaterial" && eventData != nullptr)
        {
            const auto* name = static_cast<const std::string*>(eventData);
            CreateMaterial(*name, "Shaders/StandardPBR.hlsl");
            return true;
        }
        if (eventType == "SaveMaterial")
        {
            SaveMaterial();
            return true;
        }
        if (eventType == "AssetDeleted" && eventData != nullptr)
        {
            const auto* path = static_cast<const std::string*>(eventData);
            auto it = std::find_if(m_materials.begin(), m_materials.end(),
                                   [&](const MaterialDefinition& mat) { return mat.filePath == *path; });
            if (it != m_materials.end())
            {
                int index = static_cast<int>(std::distance(m_materials.begin(), it));
                m_materials.erase(it);
                if (m_selectedMaterialIndex == index)
                {
                    m_selectedMaterialIndex = -1;
                }
                else if (m_selectedMaterialIndex > index)
                {
                    --m_selectedMaterialIndex;
                }
                return true;
            }
        }
        return false;
    }

    // ========================================================================
    // Public API
    // ========================================================================

    void MaterialEditorPanel::OpenMaterial(const std::string& materialPath)
    {
        // Check if the material is already loaded
        for (int i = 0; i < static_cast<int>(m_materials.size()); ++i)
        {
            if (m_materials[i].filePath == materialPath)
            {
                m_selectedMaterialIndex = i;
                SetVisible(true);
                std::cout << "Material Editor: selected existing material '" << m_materials[i].name << "'\n";
                return;
            }
        }

        // Load the material from disk (simulated)
        MaterialDefinition material;
        material.filePath = materialPath;

        // Extract name from path (e.g., "Assets/Materials/Brick.spkmat" -> "Brick")
        std::string nameFromPath = materialPath;
        auto lastSlash = nameFromPath.find_last_of("/\\");
        if (lastSlash != std::string::npos)
        {
            nameFromPath = nameFromPath.substr(lastSlash + 1);
        }
        auto dotPos = nameFromPath.find_last_of('.');
        if (dotPos != std::string::npos)
        {
            nameFromPath = nameFromPath.substr(0, dotPos);
        }
        material.name = nameFromPath;
        material.shaderPath = "Shaders/StandardPBR.hlsl";
        material.isModified = false;
        material.isBuiltIn = false;

        PopulateDefaultPBRParameters(material);

        m_materials.push_back(std::move(material));
        m_selectedMaterialIndex = static_cast<int>(m_materials.size()) - 1;
        SetVisible(true);

        std::cout << "Material Editor: loaded material '" << m_materials.back().name << "' from '" << materialPath
                  << "'\n";
    }

    void MaterialEditorPanel::CreateMaterial(const std::string& name, const std::string& shaderPath)
    {
        MaterialDefinition material;
        material.name = name;
        material.shaderPath = shaderPath;
        material.filePath = "Assets/Materials/" + name + ".spkmat";
        material.isModified = true;
        material.isBuiltIn = false;

        PopulateDefaultPBRParameters(material);

        m_materials.push_back(std::move(material));
        m_selectedMaterialIndex = static_cast<int>(m_materials.size()) - 1;
        SetModified(true);

        std::cout << "Material Editor: created new material '" << name << "' with shader '" << shaderPath << "'\n";
    }

    bool MaterialEditorPanel::SaveMaterial()
    {
        MaterialDefinition* selected = GetSelectedMaterial();
        if (selected == nullptr)
        {
            std::cout << "Material Editor: no material selected to save\n";
            return false;
        }

        if (selected->isBuiltIn)
        {
            std::cout << "Material Editor: cannot save built-in material '" << selected->name << "'\n";
            return false;
        }

        // Serialize material to .spkmat file (simplified text format)
        std::ofstream file(selected->filePath);
        if (!file.is_open())
        {
            std::cout << "Material Editor: failed to open file '" << selected->filePath << "' for writing\n";
            return false;
        }

        file << "# SparkEngine Material\n";
        file << "name: " << selected->name << "\n";
        file << "shader: " << selected->shaderPath << "\n";
        file << "\n";

        // Write parameters
        file << "# Parameters\n";
        for (const auto& param : selected->parameters)
        {
            file << "param " << param.name << " ";
            switch (param.type)
            {
            case ShaderParamType::Float:
                file << "float " << param.floatValues[0] << "\n";
                break;
            case ShaderParamType::Float2:
                file << "float2 " << param.floatValues[0] << " " << param.floatValues[1] << "\n";
                break;
            case ShaderParamType::Float3:
                file << "float3 " << param.floatValues[0] << " " << param.floatValues[1] << " " << param.floatValues[2]
                     << "\n";
                break;
            case ShaderParamType::Float4:
                file << "float4 " << param.floatValues[0] << " " << param.floatValues[1] << " " << param.floatValues[2]
                     << " " << param.floatValues[3] << "\n";
                break;
            case ShaderParamType::Color:
                file << "color " << param.floatValues[0] << " " << param.floatValues[1] << " " << param.floatValues[2]
                     << " " << param.floatValues[3] << "\n";
                break;
            case ShaderParamType::Int:
                file << "int " << param.intValue << "\n";
                break;
            case ShaderParamType::Bool:
                file << "bool " << (param.boolValue ? "true" : "false") << "\n";
                break;
            case ShaderParamType::Texture2D:
                file << "texture2d " << (param.texturePath.empty() ? "none" : param.texturePath) << "\n";
                break;
            case ShaderParamType::TextureCube:
                file << "texturecube " << (param.texturePath.empty() ? "none" : param.texturePath) << "\n";
                break;
            case ShaderParamType::Matrix4x4:
                file << "matrix4x4";
                for (int i = 0; i < 16; ++i)
                {
                    file << " " << param.floatValues[i];
                }
                file << "\n";
                break;
            }
        }
        file << "\n";

        // Write texture slots
        file << "# Texture Slots\n";
        for (const auto& slot : selected->textureSlots)
        {
            file << "texture_slot " << slot.name << " " << slot.bindSlot << " "
                 << (slot.texturePath.empty() ? "none" : slot.texturePath) << " " << slot.tilingU << " " << slot.tilingV
                 << " " << slot.offsetU << " " << slot.offsetV << "\n";
        }
        file << "\n";

        // Write render state
        file << "# Render State\n";
        file << "blend_mode " << static_cast<int>(selected->renderState.blendMode) << "\n";
        file << "cull_mode " << static_cast<int>(selected->renderState.cullMode) << "\n";
        file << "depth_write " << (selected->renderState.depthWrite ? "true" : "false") << "\n";
        file << "depth_test " << (selected->renderState.depthTest ? "true" : "false") << "\n";
        file << "cast_shadows " << (selected->renderState.castShadows ? "true" : "false") << "\n";
        file << "receive_shadows " << (selected->renderState.receiveShadows ? "true" : "false") << "\n";
        file << "alpha_clip " << selected->renderState.alphaClipThreshold << "\n";
        file << "render_queue " << selected->renderState.renderQueue << "\n";

        file.close();

        selected->isModified = false;
        SetModified(false);
        NotifyStateChange();

        std::cout << "Material Editor: saved material '" << selected->name << "' to '" << selected->filePath << "'\n";
        return true;
    }

    bool MaterialEditorPanel::HasUnsavedChanges() const
    {
        for (const auto& mat : m_materials)
        {
            if (mat.isModified)
            {
                return true;
            }
        }
        return false;
    }

    // ========================================================================
    // Toolbar
    // ========================================================================

    void MaterialEditorPanel::RenderToolbar()
    {
        MaterialDefinition* selected = GetSelectedMaterial();

        // Save button
        bool canSave = (selected != nullptr && selected->isModified && !selected->isBuiltIn);
        if (!canSave)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(ICON_FA_SAVE " Save"))
        {
            SaveMaterial();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Save the current material (Ctrl+S)");
        }
        if (!canSave)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();

        // Create new material button
        if (ImGui::Button(ICON_FA_PLUS " New"))
        {
            ImGui::OpenPopup("CreateMaterialPopup");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Create a new material");
        }

        // Create material popup
        if (ImGui::BeginPopup("CreateMaterialPopup"))
        {
            static char newMaterialName[128] = "NewMaterial";
            static int selectedShaderIndex = 0;

            ImGui::Text(ICON_FA_PALETTE " Create New Material");
            ImGui::Separator();
            ImGui::InputText("Name", newMaterialName, sizeof(newMaterialName));

            if (ImGui::BeginCombo("Shader", m_availableShaders[selectedShaderIndex].name.c_str()))
            {
                for (int i = 0; i < static_cast<int>(m_availableShaders.size()); ++i)
                {
                    bool isSelected = (i == selectedShaderIndex);
                    if (ImGui::Selectable(m_availableShaders[i].name.c_str(), isSelected))
                    {
                        selectedShaderIndex = i;
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", m_availableShaders[i].description.c_str());
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (ImGui::Button("Create", ImVec2(120, 0)))
            {
                CreateMaterial(newMaterialName, m_availableShaders[selectedShaderIndex].path);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        SparkEditor::VerticalSeparator();

        // Preview shape selector
        ImGui::Text("Shape:");
        ImGui::SameLine();
        const char* shapeNames[] = {"Sphere", "Cube", "Plane", "Cylinder", "Custom"};
        int currentShape = static_cast<int>(m_previewShape);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##PreviewShape", &currentShape, shapeNames, 5))
        {
            m_previewShape = static_cast<PreviewShape>(currentShape);
        }

        ImGui::SameLine();

        // Preview lighting selector
        ImGui::Text("Lighting:");
        ImGui::SameLine();
        const char* lightingNames[] = {"Default", "Scene", "IBL Only", "Unlit"};
        int currentLighting = static_cast<int>(m_previewLighting);
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##PreviewLighting", &currentLighting, lightingNames, 4))
        {
            m_previewLighting = static_cast<PreviewLighting>(currentLighting);
        }

        ImGui::SameLine();

        // Rotation toggle
        ImGui::Checkbox("Rotate", &m_previewRotate);
    }

    // ========================================================================
    // Material List
    // ========================================================================

    void MaterialEditorPanel::RenderMaterialList()
    {
        ImGui::Text(ICON_FA_PALETTE " Materials");
        ImGui::Separator();

        // Search filter
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint(
            "##MaterialSearch", ICON_FA_SEARCH " Search...", &m_searchFilter[0], m_searchFilter.capacity() + 1,
            ImGuiInputTextFlags_CallbackResize,
            [](ImGuiInputTextCallbackData* data) -> int
            {
                if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
                {
                    auto* str = static_cast<std::string*>(data->UserData);
                    str->resize(data->BufTextLen);
                    data->Buf = &(*str)[0];
                }
                return 0;
            },
            &m_searchFilter);
        ImGui::Spacing();

        // Material list with filtering
        for (int i = 0; i < static_cast<int>(m_materials.size()); ++i)
        {
            const auto& mat = m_materials[i];

            // Apply search filter (case-insensitive)
            if (!m_searchFilter.empty())
            {
                std::string nameLower = mat.name;
                std::string filterLower = m_searchFilter;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (nameLower.find(filterLower) == std::string::npos)
                {
                    continue;
                }
            }

            // Build display label
            std::string label = mat.name;
            if (mat.isModified)
            {
                label += " *";
            }
            if (mat.isBuiltIn)
            {
                label = ICON_FA_LOCK " " + label;
            }
            else
            {
                label = ICON_FA_PALETTE " " + label;
            }

            bool isSelected = (m_selectedMaterialIndex == i);
            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                m_selectedMaterialIndex = i;
            }

            // Context menu
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem(ICON_FA_COPY " Duplicate"))
                {
                    MaterialDefinition copy = mat;
                    copy.name = mat.name + " (Copy)";
                    copy.filePath = "Assets/Materials/" + copy.name + ".spkmat";
                    copy.isModified = true;
                    copy.isBuiltIn = false;
                    m_materials.push_back(std::move(copy));
                }
                if (!mat.isBuiltIn && ImGui::MenuItem(ICON_FA_TRASH " Delete"))
                {
                    m_materials.erase(m_materials.begin() + i);
                    if (m_selectedMaterialIndex >= static_cast<int>(m_materials.size()))
                    {
                        m_selectedMaterialIndex = static_cast<int>(m_materials.size()) - 1;
                    }
                    ImGui::EndPopup();
                    break;
                }
                if (ImGui::MenuItem(ICON_FA_SAVE " Save"))
                {
                    m_selectedMaterialIndex = i;
                    SaveMaterial();
                }
                ImGui::EndPopup();
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("Shader: %s", mat.shaderPath.c_str());
                ImGui::Text("Path: %s", mat.filePath.c_str());
                ImGui::Text("Parameters: %d", static_cast<int>(mat.parameters.size()));
                ImGui::Text("Textures: %d", static_cast<int>(mat.textureSlots.size()));
                ImGui::EndTooltip();
            }
        }
    }

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
            if (groups.find(group) == groups.end())
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

    // ========================================================================
    // Utility
    // ========================================================================

    MaterialDefinition* MaterialEditorPanel::GetSelectedMaterial()
    {
        if (m_selectedMaterialIndex >= 0 && m_selectedMaterialIndex < static_cast<int>(m_materials.size()))
        {
            return &m_materials[m_selectedMaterialIndex];
        }
        return nullptr;
    }

} // namespace SparkEditor
