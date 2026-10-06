/**
 * @file MaterialEditorPanel.cpp
 * @brief Visual material and shader property editor — core lifecycle and UI
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains: constructor, Initialize, Update, Render, Shutdown, HandleEvent,
 * HasUnsavedChanges,
 * GetSelectedMaterial, RenderToolbar, RenderMaterialList.
 *
 * File loading/saving lives in MaterialEditorFiles.cpp.
 * Parameter/texture editing lives in MaterialEditorParameters.cpp.
 * Preview, render state, and default materials live in MaterialEditorPreview.cpp.
 */

#include "MaterialEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Utils/ImGuiUtils.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include "Utils/FileUtils.h"
#include "Utils/LogMacros.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>

namespace SparkEditor
{

    // ========================================================================
    // Construction / Lifecycle
    // ========================================================================

    MaterialEditorPanel::MaterialEditorPanel() : EditorPanel("Material Editor", "material_editor_panel")
    {
        SetSize(900.0f, 650.0f);
    }

    bool MaterialEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Material Editor panel");

        SetIcon(ICON_FA_PALETTE);

        PopulateAvailableShaders();

        LoadDefaultMaterials();

        m_isInitialized = true;
        return true;
    }

    void MaterialEditorPanel::PopulateAvailableShaders()
    {
        m_availableShaders.clear();

        // Enumerate the shader tree that actually ships instead of advertising fixed filenames. The staged
        // layout puts shaders next to the executable (bin/<config>/Shaders); a source-tree run finds them
        // under SparkEngine/Shaders. Every entry therefore names a file the engine can load.
        const std::array<std::filesystem::path, 2> roots = {std::filesystem::path("Shaders"),
                                                            std::filesystem::path("SparkEngine/Shaders")};
        for (const auto& root : roots)
        {
            std::error_code rootError;
            if (!std::filesystem::is_directory(root, rootError) || rootError)
                continue;

            std::error_code walkError;
            std::filesystem::recursive_directory_iterator it(
                root, std::filesystem::directory_options::skip_permission_denied, walkError);
            const std::filesystem::recursive_directory_iterator end;
            for (; !walkError && it != end; it.increment(walkError))
            {
                std::error_code entryError;
                if (!it->is_regular_file(entryError) || entryError)
                    continue;
                if (it->path().extension() != ".hlsl")
                    continue;

                // info.path is handed to narrow std::string shader loading. A name the
                // Windows ANSI code page cannot spell has no such path, and
                // path::string() throws for it (which ended the scan): skip it. Name and
                // description are ImGui text, so UTF-8.
                if (!Spark::FileUtils::TryPathToNarrow(it->path()))
                {
                    continue;
                }

                ShaderInfo info;
                info.name = Spark::FileUtils::TryPathToUtf8(it->path().stem()).value_or("?");
                info.path = it->path().generic_string();
                info.description = Spark::FileUtils::TryPathToUtf8(it->path().parent_path()).value_or(info.path);
                m_availableShaders.push_back(std::move(info));
            }

            if (!m_availableShaders.empty())
                break;
        }

        std::sort(m_availableShaders.begin(), m_availableShaders.end(),
                  [](const ShaderInfo& a, const ShaderInfo& b) { return a.name < b.name; });

        if (m_availableShaders.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "Material Editor found no .hlsl files under Shaders/ or SparkEngine/Shaders; the shader "
                           "picker will be empty");
        }
        else
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Material Editor found %zu shader(s)",
                           m_availableShaders.size());
        }
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Material Editor panel");
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

            // The shader list is enumerated from disk, so it can legitimately be empty.
            if (m_availableShaders.empty())
            {
                ImGui::TextWrapped("No shaders found under Shaders/ or SparkEngine/Shaders.");
            }
            else
            {
                if (selectedShaderIndex >= static_cast<int>(m_availableShaders.size()))
                    selectedShaderIndex = 0;

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
            }

            ImGui::BeginDisabled(m_availableShaders.empty());
            if (ImGui::Button("Create", ImVec2(120, 0)))
            {
                CreateMaterial(newMaterialName, m_availableShaders[selectedShaderIndex].path);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
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
