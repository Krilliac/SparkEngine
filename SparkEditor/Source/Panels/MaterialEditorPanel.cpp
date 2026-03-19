/**
 * @file MaterialEditorPanel.cpp
 * @brief Visual material and shader property editor — core lifecycle and UI
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains: constructor, Initialize, Update, Render, Shutdown, HandleEvent,
 * OpenMaterial, CreateMaterial, SaveMaterial, HasUnsavedChanges,
 * GetSelectedMaterial, RenderToolbar, RenderMaterialList.
 *
 * Parameter/texture editing lives in MaterialEditorParameters.cpp.
 * Preview, render state, and default materials live in MaterialEditorPreview.cpp.
 */

#include "MaterialEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
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
                if (!nameLower.contains(filterLower))
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
