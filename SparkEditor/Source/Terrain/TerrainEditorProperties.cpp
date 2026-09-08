/**
 * @file TerrainEditorProperties.cpp
 * @brief Terrain property, layer, detail, and generation ImGui panels
 */

#include "TerrainEditor.h"
#include "../Core/EditorIcons.h"

#include <imgui.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace SparkEditor
{

    void TerrainEditor::RenderTerrainProperties()
    {
        if (ImGui::CollapsingHeader(ICON_FA_COG " Terrain Properties"))
        {
            if (!m_currentTerrain)
                return;

            ImGui::Indent(8.0f);

            char nameBuffer[256] = {};
            std::strncpy(nameBuffer, m_currentTerrain->name.c_str(), sizeof(nameBuffer) - 1);
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
            {
                m_currentTerrain->name = nameBuffer;
                SetModified(true);
            }

            ImGui::DragFloat("Size", &m_currentTerrain->size, 10.0f, 100.0f, 10000.0f, "%.0f m");
            ImGui::DragFloat3("Position", &m_currentTerrain->position.x, 1.0f);

            ImGui::Separator();
            ImGui::Text("LOD:");
            ImGui::DragInt("LOD Levels", &m_currentTerrain->lodLevels, 0.1f, 1, 8);
            ImGui::DragFloat("LOD Bias", &m_currentTerrain->lodBias, 0.1f, 0.1f, 4.0f, "%.1f");

            ImGui::Separator();
            ImGui::Text("Physics:");
            ImGui::Checkbox("Generate Collider", &m_currentTerrain->generateCollider);

            ImGui::Separator();

            // Save target. Defaults to the same project-relative location the Load field starts from, so a
            // saved terrain is findable; the field is editable and an existing file must be confirmed.
            if (m_savePathBuffer[0] == '\0')
            {
                RefreshPathBuffers();
            }
            ImGui::InputText("Save Path", m_savePathBuffer, sizeof(m_savePathBuffer));

            // SaveTerrain() refuses a terrain the readers would reject; that verdict must reach the user
            // instead of leaving the button looking like it worked.
            const auto saveNow = [this]()
            {
                if (SaveTerrain(m_savePathBuffer))
                {
                    m_terrainIoMessage.clear();
                }
                else
                {
                    m_terrainIoMessage = std::string("Failed to save terrain to: ") + m_savePathBuffer;
                }
            };

            if (ImGui::Button(ICON_FA_SAVE " Save Terrain", ImVec2(-1, 28)))
            {
                std::error_code existsError;
                if (std::filesystem::exists(m_savePathBuffer, existsError))
                    ImGui::OpenPopup("Overwrite Terrain?");
                else
                    saveNow();
            }

            if (ImGui::BeginPopupModal("Overwrite Terrain?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextWrapped("%s already exists. Overwrite it?", m_savePathBuffer);
                ImGui::Separator();
                if (ImGui::Button("Overwrite", ImVec2(120, 0)))
                {
                    saveNow();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            if (!m_terrainIoMessage.empty())
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_terrainIoMessage.c_str());
            }

            ImGui::Unindent(8.0f);
        }
    }

    void TerrainEditor::RenderTextureLayersPanel()
    {
        if (!m_currentTerrain)
            return;

        ImGui::Text("Texture Layers (%d)", static_cast<int>(m_currentTerrain->textureLayers.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Layer"))
        {
            // AddTextureLayer refuses past the .sparkterrain layer limit; a silently dead button would
            // look identical to a layer that was added.
            if (m_currentTerrain->AddTextureLayer("New Layer") != nullptr)
            {
                m_terrainIoMessage.clear();
                SetModified(true);
            }
            else
            {
                m_terrainIoMessage = "Cannot add another texture layer: the .sparkterrain limit was reached.";
            }
        }

        ImGui::BeginChild("##TextureLayers", ImVec2(0, 150), true);
        int removeIdx = -1;
        for (int i = 0; i < static_cast<int>(m_currentTerrain->textureLayers.size()); ++i)
        {
            auto& layer = m_currentTerrain->textureLayers[static_cast<size_t>(i)];
            ImGui::PushID(i);

            bool selected = (m_selectedTextureLayer == i);
            if (ImGui::Selectable(layer->name.c_str(), selected))
            {
                m_selectedTextureLayer = i;
            }

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Tiling: %.1f x %.1f\nOpacity: %.0f%%", layer->tiling.x, layer->tiling.y,
                                  layer->opacity * 100.0f);
            }

            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Remove"))
                    removeIdx = i;
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndChild();

        if (removeIdx >= 0)
        {
            m_currentTerrain->RemoveTextureLayer(removeIdx);
            SetModified(true);
        }

        if (m_selectedTextureLayer >= 0 &&
            m_selectedTextureLayer < static_cast<int>(m_currentTerrain->textureLayers.size()))
        {
            auto& layer = m_currentTerrain->textureLayers[static_cast<size_t>(m_selectedTextureLayer)];
            ImGui::Separator();

            // Texture bindings. Without these the layer only ever had a display name, so nothing the
            // runtime could resolve was ever authored, saved or synced.
            char diffuseBuffer[256] = {};
            std::strncpy(diffuseBuffer, layer->diffuseTexture.c_str(), sizeof(diffuseBuffer) - 1);
            if (ImGui::InputText("Diffuse Texture", diffuseBuffer, sizeof(diffuseBuffer)))
            {
                layer->diffuseTexture = diffuseBuffer;
                SetModified(true);
            }

            char normalBuffer[256] = {};
            std::strncpy(normalBuffer, layer->normalTexture.c_str(), sizeof(normalBuffer) - 1);
            if (ImGui::InputText("Normal Texture", normalBuffer, sizeof(normalBuffer)))
            {
                layer->normalTexture = normalBuffer;
                SetModified(true);
            }

            ImGui::DragFloat2("Tiling", &layer->tiling.x, 0.1f, 0.01f, 100.0f);
            ImGui::DragFloat("Opacity", &layer->opacity, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Metallic", &layer->metallic, 0.01f, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat("Roughness", &layer->roughness, 0.01f, 0.0f, 1.0f, "%.2f");

            ImGui::Separator();
            ImGui::Checkbox("Auto Placement", &layer->useAutoPlacement);
            if (layer->useAutoPlacement)
            {
                ImGui::DragFloatRange2("Height", &layer->minHeight, &layer->maxHeight, 0.5f, 0.0f, 1000.0f);
                ImGui::DragFloatRange2("Slope", &layer->minSlope, &layer->maxSlope, 0.5f, 0.0f, 90.0f);
                if (ImGui::Button("Apply Auto-Placement"))
                {
                    AutoGenerateTexturePlacement(m_selectedTextureLayer);
                }
            }
        }
    }

    void TerrainEditor::RenderDetailMeshesPanel()
    {
        if (!m_currentTerrain)
            return;

        ImGui::Text("Detail Meshes (%d)", static_cast<int>(m_currentTerrain->detailMeshes.size()));

        if (ImGui::Button(ICON_FA_PLUS " Add Detail"))
        {
            auto detail = std::make_unique<TerrainDetailMesh>();
            detail->name = "New Detail";
            m_currentTerrain->detailMeshes.push_back(std::move(detail));
            SetModified(true);
        }

        ImGui::BeginChild("##DetailMeshes", ImVec2(0, 120), true);
        for (int i = 0; i < static_cast<int>(m_currentTerrain->detailMeshes.size()); ++i)
        {
            auto& detail = m_currentTerrain->detailMeshes[static_cast<size_t>(i)];
            ImGui::PushID(i);
            bool selected = (m_selectedDetailMesh == i);
            if (ImGui::Selectable(detail->name.c_str(), selected))
            {
                m_selectedDetailMesh = i;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        if (m_selectedDetailMesh >= 0 && m_selectedDetailMesh < static_cast<int>(m_currentTerrain->detailMeshes.size()))
        {
            auto& detail = m_currentTerrain->detailMeshes[static_cast<size_t>(m_selectedDetailMesh)];

            char meshPathBuffer[256] = {};
            std::strncpy(meshPathBuffer, detail->meshPath.c_str(), sizeof(meshPathBuffer) - 1);
            if (ImGui::InputText("Mesh Path", meshPathBuffer, sizeof(meshPathBuffer)))
            {
                detail->meshPath = meshPathBuffer;
                SetModified(true);
            }

            ImGui::DragFloat("Density", &detail->density, 0.1f, 0.1f, 10.0f);
            ImGui::DragFloat("View Distance", &detail->viewDistance, 1.0f, 10.0f, 500.0f);
            ImGui::DragFloat2("Scale Range", &detail->scaleRange.x, 0.05f, 0.1f, 5.0f);
            ImGui::Checkbox("Cast Shadows", &detail->castShadows);
        }
    }

    void TerrainEditor::RenderGenerationTools()
    {
        if (ImGui::CollapsingHeader(ICON_FA_COGS " Procedural Generation", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent(8.0f);

            ImGui::TextDisabled("Noise");
            ImGui::DragInt("Octaves", &m_generationParams.noiseOctaves, 0.1f, 1, 12);
            ImGui::DragFloat("Frequency", &m_generationParams.noiseFrequency, 0.001f, 0.001f, 1.0f, "%.3f");
            ImGui::DragFloat("Amplitude", &m_generationParams.noiseAmplitude, 1.0f, 1.0f, 500.0f, "%.0f");
            ImGui::DragFloat("Lacunarity", &m_generationParams.noiseLacunarity, 0.1f, 1.0f, 4.0f, "%.1f");
            ImGui::DragFloat("Persistence", &m_generationParams.noisePersistence, 0.01f, 0.0f, 1.0f, "%.2f");

            if (ImGui::Button(ICON_FA_BOLT " Generate Heightmap", ImVec2(-1, 28)))
            {
                GenerateNoiseHeightmap(m_generationParams.noiseOctaves, m_generationParams.noiseFrequency,
                                       m_generationParams.noiseAmplitude, m_generationParams.noiseLacunarity,
                                       m_generationParams.noisePersistence);
            }

            ImGui::Separator();
            ImGui::TextDisabled("Post-Process");

            ImGui::DragInt("Smooth Iterations", &m_generationParams.smoothIterations, 0.1f, 1, 20);
            ImGui::DragFloat("Smooth Strength", &m_generationParams.smoothStrength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button(ICON_FA_MAGIC " Smooth", ImVec2(-1, 28)))
            {
                SmoothTerrain(m_generationParams.smoothIterations, m_generationParams.smoothStrength);
            }

            ImGui::Separator();
            ImGui::DragInt("Erosion Iterations", &m_generationParams.erosionIterations, 1.0f, 10, 10000);
            ImGui::DragFloat("Erosion Strength", &m_generationParams.erosionStrength, 0.01f, 0.0f, 1.0f, "%.2f");
            if (ImGui::Button(ICON_FA_FIRE " Apply Erosion", ImVec2(-1, 28)))
            {
                ApplyErosion(m_generationParams.erosionIterations, m_generationParams.erosionStrength,
                             m_generationParams.evaporationRate, m_generationParams.depositionRate);
            }

            ImGui::Unindent(8.0f);
        }
    }

} // namespace SparkEditor
