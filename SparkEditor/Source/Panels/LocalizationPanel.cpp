/**
 * @file LocalizationPanel.cpp
 * @brief Implementation of the localization editor panel
 */

#include "LocalizationPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

namespace SparkEditor
{

    LocalizationPanel::LocalizationPanel() : EditorPanel("Localization", "localization_panel") {}

    bool LocalizationPanel::Initialize()
    {
        std::cout << "Initializing Localization panel\n";
        return true;
    }

    void LocalizationPanel::Update(float /*deltaTime*/) {}

    void LocalizationPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderLanguageSelector();
            ImGui::Separator();
            RenderStringTable();
            ImGui::Separator();
            RenderAddEntryForm();
        }
        EndPanel();
    }

    void LocalizationPanel::Shutdown()
    {
        std::cout << "Shutting down Localization panel\n";
    }

    void LocalizationPanel::RenderLanguageSelector()
    {
        ImGui::Text(ICON_FA_GLOBE " Languages:");
        ImGui::SameLine();

        const char* langNames[] = {"English", "Spanish", "French", "German", "Japanese", "Chinese"};
        ImGui::SetNextItemWidth(150);
        ImGui::Combo("##Language", &m_currentLanguage, langNames, IM_ARRAYSIZE(langNames));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText(ICON_FA_FILTER " Filter", m_filterText, sizeof(m_filterText));

        ImGui::SameLine();
        ImGui::Text("Strings: %d", static_cast<int>(m_strings.size()));
    }

    void LocalizationPanel::RenderStringTable()
    {
        ImGui::BeginChild("StringTable", ImVec2(0, -80), true);

        int columnCount = 2 + static_cast<int>(m_languages.size());
        if (ImGui::BeginTable("L10NTable", columnCount,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 200);
            ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, 30);
            for (const auto& lang : m_languages)
                ImGui::TableSetupColumn(lang.c_str());
            ImGui::TableHeadersRow();

            bool hasFilter = strlen(m_filterText) > 0;

            for (int i = 0; i < static_cast<int>(m_strings.size()); ++i)
            {
                auto& entry = m_strings[i];

                if (hasFilter && !entry.key.contains(m_filterText))
                    continue;

                ImGui::PushID(i);
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.key.c_str());

                ImGui::TableNextColumn();
                if (ImGui::SmallButton(ICON_FA_TRASH))
                {
                    m_strings.erase(m_strings.begin() + i);
                    --i;
                    ImGui::PopID();
                    continue;
                }

                for (const auto& lang : m_languages)
                {
                    ImGui::TableNextColumn();
                    char buf[512] = {};
                    auto it = entry.translations.find(lang);
                    if (it != entry.translations.end())
                        strncpy(buf, it->second.c_str(), sizeof(buf) - 1);

                    char widgetId[32];
                    snprintf(widgetId, sizeof(widgetId), "##%s", lang.c_str());
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText(widgetId, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                        entry.translations[lang] = buf;
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        ImGui::EndChild();
    }

    void LocalizationPanel::RenderAddEntryForm()
    {
        ImGui::TextDisabled("Add New Entry");
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Key##New", m_newKey, sizeof(m_newKey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("Value (default lang)##New", m_newValue, sizeof(m_newValue));
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_PLUS " Add") && strlen(m_newKey) > 0)
        {
            LocalizedString entry;
            entry.key = m_newKey;
            if (m_currentLanguage < static_cast<int>(m_languages.size()))
                entry.translations[m_languages[m_currentLanguage]] = m_newValue;

            m_strings.push_back(entry);
            m_newKey[0] = '\0';
            m_newValue[0] = '\0';
        }
    }

} // namespace SparkEditor
