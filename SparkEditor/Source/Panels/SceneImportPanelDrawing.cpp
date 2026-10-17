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

#include "../Core/EditorIcons.h"
#include "../Core/EditorUI.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

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
            if (Spark::Ui::BeginPanelToolbar("##SceneImportToolbar"))
            {
                if (Spark::Ui::SecondaryButton(ICON_FA_REFRESH " Refresh"))
                {
                    ScanSceneFiles();
                }
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Import (one-way)");
                Spark::Ui::EndPanelToolbar();
            }
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::SecondaryText("Imports game .scene (INI) objects as normal, editable entities in the current "
                                     "world. Saving writes the editor's JSON scene format — nothing is ever written "
                                     "back to the .scene source.");
            ImGui::PopTextWrapPos();
            ImGui::Separator();

            const float listWidth = Spark::Ui::Px(260.0f);
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
            const std::string hint = "Add .scene files under '" + m_assetsPrefix + "Assets/Scenes', then Refresh.";
            if (Spark::Ui::EmptyState(ICON_FA_FILE, "No .scene files found", hint.c_str(), "Refresh"))
            {
                ScanSceneFiles();
            }
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
            Spark::Ui::EmptyState(ICON_FA_FILE_IMPORT, "No scene selected",
                                  "Select a .scene file to preview and import.");
        }
        else if (!m_previewValid)
        {
            ImGui::PushTextWrapPos(0.0f);
            Spark::Ui::StatusText(Spark::Ui::Status::Error, "Cannot open '%s'.",
                                  m_sceneFiles[static_cast<size_t>(m_selected)].diskPath.c_str());
            ImGui::PopTextWrapPos();
        }
        else
        {
            const SceneFileEntry& entry = m_sceneFiles[static_cast<size_t>(m_selected)];

            if (Spark::Ui::BeginPropertyGrid("SceneImportPreview"))
            {
                Spark::Ui::PropertyLabel("Scene");
                ImGui::TextUnformatted(m_preview.sceneName.empty() ? "(unnamed)" : m_preview.sceneName.c_str());
                Spark::Ui::PropertyLabel("File");
                Spark::Ui::MonoText("%s", entry.diskPath.c_str());
                Spark::Ui::PropertyLabel("Importable objects");
                Spark::Ui::MonoText("%d", static_cast<int>(m_preview.objects.size()));
                Spark::Ui::PropertyLabel("Skipped nodes");
                Spark::Ui::MonoText("%d", static_cast<int>(m_preview.skippedTypes.size()));
                Spark::Ui::EndPropertyGrid();
            }
            if (!m_preview.skippedTypes.empty())
            {
                for (const std::string& skip : AggregateSkipped(m_preview.skippedTypes))
                    ImGui::BulletText("%s", skip.c_str());
            }
            if (!m_preview.unresolvedModels.empty())
            {
                ImGui::Spacing();
                Spark::Ui::StatusText(Spark::Ui::Status::Warning,
                                      "Unresolved model paths (%d) - will render as placeholder cubes:",
                                      static_cast<int>(m_preview.unresolvedModels.size()));
                for (const std::string& model : m_preview.unresolvedModels)
                    ImGui::BulletText("%s", model.c_str());
            }

            ImGui::Spacing();
            const bool haveWorld = (m_editorUI != nullptr && m_editorUI->GetWorld() != nullptr);
            const bool canImport = haveWorld && !m_preview.objects.empty();
            const char* blockedReason = nullptr;
            if (!haveWorld)
                blockedReason = "No document World available (EditorUI not wired).";
            else if (m_preview.objects.empty())
                blockedReason = "Nothing importable in this file.";
            {
                const Spark::Ui::ScopedDisabled disabled(!canImport, blockedReason);
                if (Spark::Ui::PrimaryButton(ICON_FA_FILE_IMPORT " Import Into Current World"))
                {
                    ImportParsed(m_preview, entry.displayPath);
                }
            }
            if (blockedReason != nullptr)
                Spark::Ui::SecondaryText("%s", blockedReason);
            ImGui::SameLine();
            Spark::Ui::SecondaryText("(undoable as one step: Ctrl+Z removes the whole import)");
        }

        if (m_hasImported)
        {
            Spark::Ui::SubHeader("Last import");
            if (Spark::Ui::BeginPropertyGrid("SceneImportLast"))
            {
                Spark::Ui::PropertyLabel("Source");
                Spark::Ui::MonoText("%s", m_lastImport.sourcePath.c_str());
                Spark::Ui::PropertyLabel("Entities created");
                Spark::Ui::MonoText("%d", m_lastImport.imported);
                Spark::Ui::EndPropertyGrid();
            }
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
                Spark::Ui::StatusText(Spark::Ui::Status::Warning, "Unresolved model paths (placeholder cubes):");
                for (const std::string& model : m_lastImport.unresolvedModels)
                    ImGui::BulletText("%s", model.c_str());
            }

            ImGui::Spacing();
            Spark::Ui::SecondaryText("One-way import: saving writes the editor's JSON format, not .scene.");
            ImGui::Spacing();
            if (Spark::Ui::SecondaryButton("Close", Spark::Ui::Px2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

} // namespace SparkEditor
