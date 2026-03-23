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
            RenderToolbar();
            ImGui::Separator();
            RenderLanguageSelector();
            ImGui::Separator();
            RenderCoverageMetrics();
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

    void LocalizationPanel::RenderToolbar()
    {
        if (ImGui::Button(ICON_FA_FILE_IMPORT " Import CSV"))
        {
            std::cout << "Import CSV placeholder\n";
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE_EXPORT " Export CSV"))
        {
            std::cout << "Export CSV placeholder\n";
        }
        ImGui::SameLine();
        ImGui::Checkbox("Show Missing Only", &m_showMissingOnly);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragInt("Char Warn", &m_charLengthWarning, 1.0f, 10, 1000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Warn when string exceeds this character count");
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

    void LocalizationPanel::RenderCoverageMetrics()
    {
        if (m_strings.empty())
        {
            ImGui::TextDisabled("No strings to analyze.");
            return;
        }

        ImGui::Text(ICON_FA_CHART_BAR " Coverage:");
        int totalStrings = static_cast<int>(m_strings.size());

        for (int l = 0; l < static_cast<int>(m_languages.size()); ++l)
        {
            const auto& lang = m_languages[l];
            int translated = 0;
            for (const auto& entry : m_strings)
            {
                auto it = entry.translations.find(lang);
                if (it != entry.translations.end() && !it->second.empty())
                    ++translated;
            }

            float coverage = totalStrings > 0 ? static_cast<float>(translated) / totalStrings : 0.0f;
            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%s: %d/%d (%.0f%%)", lang.c_str(), translated, totalStrings,
                     coverage * 100.0f);

            ImVec4 barColor = coverage >= 1.0f  ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                              : coverage > 0.5f ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                                : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColor);
            ImGui::ProgressBar(coverage, ImVec2(250, 0), overlay);
            ImGui::PopStyleColor();

            if (l % 3 != 2 && l < static_cast<int>(m_languages.size()) - 1)
                ImGui::SameLine();
        }
    }

    void LocalizationPanel::RenderStringTable()
    {
        ImGui::BeginChild("StringTable", ImVec2(0, -80), true);

        // Columns: Key, Context, then one per language
        int columnCount = 2 + static_cast<int>(m_languages.size());
        if (ImGui::BeginTable("L10NTable", columnCount,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY))
        {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthFixed, 120);
            for (const auto& lang : m_languages)
                ImGui::TableSetupColumn(lang.c_str());
            ImGui::TableHeadersRow();

            bool hasFilter = strlen(m_filterText) > 0;

            for (int i = 0; i < static_cast<int>(m_strings.size()); ++i)
            {
                auto& entry = m_strings[i];

                if (hasFilter && !entry.key.contains(m_filterText))
                    continue;

                // Missing-only filter
                if (m_showMissingOnly)
                {
                    bool allPresent = true;
                    for (const auto& lang : m_languages)
                    {
                        auto it = entry.translations.find(lang);
                        if (it == entry.translations.end() || it->second.empty())
                        {
                            allPresent = false;
                            break;
                        }
                    }
                    if (allPresent)
                        continue;
                }

                ImGui::PushID(i);
                ImGui::TableNextRow();

                // Key
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.key.c_str());

                // Context
                ImGui::TableNextColumn();
                char ctxBuf[256] = {};
                strncpy(ctxBuf, entry.context.c_str(), sizeof(ctxBuf) - 1);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##ctx", ctxBuf, sizeof(ctxBuf), ImGuiInputTextFlags_EnterReturnsTrue))
                    entry.context = ctxBuf;

                // Language columns
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

                    // Color the background if missing
                    bool isMissing = (strlen(buf) == 0);
                    if (isMissing)
                        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.5f, 0.1f, 0.1f, 0.5f));

                    if (ImGui::InputText(widgetId, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue))
                        entry.translations[lang] = buf;

                    if (isMissing)
                        ImGui::PopStyleColor();

                    // Character length warning
                    if (!isMissing && static_cast<int>(strlen(buf)) > m_charLengthWarning)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "!");
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("String exceeds %d characters (%d)", m_charLengthWarning,
                                              static_cast<int>(strlen(buf)));
                    }
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
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Key##New", m_newKey, sizeof(m_newKey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        ImGui::InputText("Value##New", m_newValue, sizeof(m_newValue));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("Context##New", m_newContext, sizeof(m_newContext));
        ImGui::SameLine();

        if (ImGui::Button(ICON_FA_PLUS " Add") && strlen(m_newKey) > 0)
        {
            LocalizedString entry;
            entry.key = m_newKey;
            entry.context = m_newContext;
            if (m_currentLanguage < static_cast<int>(m_languages.size()))
                entry.translations[m_languages[m_currentLanguage]] = m_newValue;

            m_strings.push_back(entry);
            m_newKey[0] = '\0';
            m_newValue[0] = '\0';
            m_newContext[0] = '\0';
        }
    }

} // namespace SparkEditor
