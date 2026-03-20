/**
 * @file ScriptEditorPanel.cpp
 * @brief Implementation of the AngelScript editor panel
 */

#include "ScriptEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>

namespace SparkEditor
{

    ScriptEditorPanel::ScriptEditorPanel() : EditorPanel("Script Editor", "script_editor_panel") {}

    bool ScriptEditorPanel::Initialize()
    {
        std::cout << "Initializing Script Editor panel\n";
        return true;
    }

    void ScriptEditorPanel::Update(float /*deltaTime*/) {}

    void ScriptEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            if (ImGui::BeginTabBar("ScriptEditorTabs"))
            {
                if (ImGui::BeginTabItem(ICON_FA_CODE " Modules"))
                {
                    RenderModuleList();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_REFRESH " Hot Reload"))
                {
                    RenderHotReloadStatus();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_BOLT " Hooks"))
                {
                    RenderHookListeners();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(ICON_FA_TERMINAL " Output"))
                {
                    RenderScriptConsole();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        EndPanel();
    }

    void ScriptEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Script Editor panel\n";
    }

    void ScriptEditorPanel::RenderModuleList()
    {
        ImGui::InputTextWithHint("##filter", "Filter modules...", m_scriptFilter, sizeof(m_scriptFilter));

        if (ImGui::Button(ICON_FA_REFRESH " Recompile All"))
        {
            m_compileLog.push_back("Recompile triggered...");
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " New Script"))
        {
            m_compileLog.push_back("New script creation...");
        }

        ImGui::Separator();

        if (m_modules.empty())
        {
            ImGui::TextDisabled("No script modules loaded.");
            ImGui::TextDisabled("Place .as files in Assets/Scripts/ and enter Play mode.");
            return;
        }

        if (ImGui::BeginTable("ModuleTable", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Module");
            ImGui::TableSetupColumn("File");
            ImGui::TableSetupColumn("Entities", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(m_modules.size()); ++i)
            {
                auto& mod = m_modules[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                bool selected = (m_selectedModule == i);
                if (ImGui::Selectable(mod.name, selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedModule = i;

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(mod.filePath);
                ImGui::TableNextColumn();
                ImGui::Text("%d", mod.entityCount);
                ImGui::TableNextColumn();
                ImGui::TextColored(mod.compiled ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f) : ImVec4(0.8f, 0.2f, 0.2f, 1.0f),
                                   mod.compiled ? "OK" : "ERR");
            }
            ImGui::EndTable();
        }
    }

    void ScriptEditorPanel::RenderHotReloadStatus()
    {
        ImGui::Checkbox("Enable Hot Reload", &m_hotReloadEnabled);
        ImGui::Separator();

        ImGui::Text("Watched Files: %d", m_watchedFiles);
        ImGui::Text("Recompile Count: %d", m_recompileCount);
        ImGui::Text("Error Count: %d", m_errorCount);

        ImGui::Separator();
        ImGui::TextDisabled("Hot reload watches Assets/Scripts/ for .as file changes.");
        ImGui::TextDisabled("Modified scripts are automatically recompiled and reattached.");
    }

    void ScriptEditorPanel::RenderHookListeners()
    {
        if (m_hooks.empty())
        {
            ImGui::TextDisabled("No script hooks registered.");
            ImGui::TextDisabled("Scripts register hooks via ScriptHookManager (28 hook types).");
            return;
        }

        if (ImGui::BeginTable("HookTable", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("Hook Type");
            ImGui::TableSetupColumn("Script");
            ImGui::TableSetupColumn("Priority", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();

            for (const auto& hook : m_hooks)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hook.hookType);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(hook.scriptName);
                ImGui::TableNextColumn();
                ImGui::Text("%d", hook.priority);
            }
            ImGui::EndTable();
        }
    }

    void ScriptEditorPanel::RenderScriptConsole()
    {
        if (ImGui::Button(ICON_FA_TRASH " Clear"))
            m_compileLog.clear();

        ImGui::Separator();

        ImGui::BeginChild("ScriptOutput", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : m_compileLog)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (!m_compileLog.empty())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

} // namespace SparkEditor
