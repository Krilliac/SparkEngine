/**
 * @file SceneImportPanelDrawing.cpp
 * @brief ImGui drawing for the one-way .scene import panel (W10)
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains: Render, RenderFileList, RenderPreviewAndImport, and
 * RenderSummaryPopup. Discovery, parsing, and import execution live in the
 * sibling SceneImportPanel.cpp.
 */

#include "SceneImportPanel.h"

#include "../Core/EditorUI.h"

#include <imgui.h>

#include <string>

namespace SparkEditor
{

    void SceneImportPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::Button("Refresh"))
            {
                ScanSceneFiles();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Import (one-way)");
            ImGui::TextWrapped("Imports game .scene (INI) objects as normal, editable entities in the current "
                               "world. Saving writes the editor's JSON scene format — nothing is ever written back "
                               "to the .scene source.");
            ImGui::Separator();

            const float listWidth = 260.0f;
            ImGui::BeginChild("SceneImportList", ImVec2(listWidth, 0), true);
            RenderFileList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("SceneImportDetails", ImVec2(0, 0), true);
            RenderPreviewAndImport();
            ImGui::EndChild();

            RenderSummaryPopup();
        }
        EndPanel();
    }

    void SceneImportPanel::RenderFileList()
    {
        if (m_sceneFiles.empty())
        {
            ImGui::TextWrapped("No .scene files found under '%sAssets/Scenes'.", m_assetsPrefix.c_str());
            return;
        }

        for (size_t i = 0; i < m_sceneFiles.size(); ++i)
        {
            const bool selected = (static_cast<int>(i) == m_selected);
            if (ImGui::Selectable(m_sceneFiles[i].displayPath.c_str(), selected))
            {
                m_selected = static_cast<int>(i);
                m_previewValid = ParseSceneFile(m_sceneFiles[i].diskPath, m_preview);
            }
        }
    }

    void SceneImportPanel::RenderPreviewAndImport()
    {
        if (m_selected < 0 || m_selected >= static_cast<int>(m_sceneFiles.size()))
        {
            ImGui::TextDisabled("Select a .scene file to preview and import.");
        }
        else if (!m_previewValid)
        {
            ImGui::TextWrapped("Cannot open '%s'.", m_sceneFiles[static_cast<size_t>(m_selected)].diskPath.c_str());
        }
        else
        {
            const SceneFileEntry& entry = m_sceneFiles[static_cast<size_t>(m_selected)];

            ImGui::Text("Scene: %s", m_preview.sceneName.empty() ? "(unnamed)" : m_preview.sceneName.c_str());
            ImGui::TextDisabled("%s", entry.diskPath.c_str());
            ImGui::Spacing();
            ImGui::Text("Importable objects: %d", static_cast<int>(m_preview.objects.size()));
            ImGui::Text("Skipped nodes: %d", static_cast<int>(m_preview.skippedTypes.size()));
            if (!m_preview.skippedTypes.empty())
            {
                for (const std::string& skip : AggregateSkipped(m_preview.skippedTypes))
                    ImGui::BulletText("%s", skip.c_str());
            }
            if (!m_preview.unresolvedModels.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   "Unresolved model paths (%d) - will render as placeholder cubes:",
                                   static_cast<int>(m_preview.unresolvedModels.size()));
                for (const std::string& model : m_preview.unresolvedModels)
                    ImGui::BulletText("%s", model.c_str());
            }

            ImGui::Spacing();
            const bool haveWorld = (m_editorUI != nullptr && m_editorUI->GetWorld() != nullptr);
            const bool canImport = haveWorld && !m_preview.objects.empty();
            if (!canImport)
                ImGui::BeginDisabled();
            if (ImGui::Button("Import Into Current World"))
            {
                ImportParsed(m_preview, entry.displayPath);
            }
            if (!canImport)
                ImGui::EndDisabled();
            if (!haveWorld)
                ImGui::TextDisabled("No document World available (EditorUI not wired).");
            else if (m_preview.objects.empty())
                ImGui::TextDisabled("Nothing importable in this file.");
            ImGui::SameLine();
            ImGui::TextDisabled("(undoable as one step: Ctrl+Z removes the whole import)");
        }

        if (m_hasImported)
        {
            ImGui::Separator();
            ImGui::Text("Last import: %s", m_lastImport.sourcePath.c_str());
            ImGui::Text("Entities created: %d", m_lastImport.imported);
        }
    }

    void SceneImportPanel::RenderSummaryPopup()
    {
        if (m_openSummaryPopup)
        {
            ImGui::OpenPopup("Scene Import Summary");
            m_openSummaryPopup = false;
        }

        if (ImGui::BeginPopupModal("Scene Import Summary", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Source: %s", m_lastImport.sourcePath.c_str());
            ImGui::Text("Imported: %d objects", m_lastImport.imported);

            ImGui::Text("Skipped: %d nodes", m_lastImport.skippedTotal);
            for (const std::string& skip : m_lastImport.skippedCounts)
                ImGui::BulletText("%s", skip.c_str());

            if (!m_lastImport.unresolvedModels.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Unresolved model paths (placeholder cubes):");
                for (const std::string& model : m_lastImport.unresolvedModels)
                    ImGui::BulletText("%s", model.c_str());
            }

            ImGui::Spacing();
            ImGui::TextDisabled("One-way import: saving writes the editor's JSON format, not .scene.");
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

} // namespace SparkEditor
