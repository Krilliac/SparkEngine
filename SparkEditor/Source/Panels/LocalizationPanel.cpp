/**
 * @file LocalizationPanel.cpp
 * @brief Implementation of the localization editor panel
 */

#include "LocalizationPanel.h"
#include "../Core/EditorIcons.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"
#include "Utils/LogMacros.h"
#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace SparkEditor
{

    LocalizationPanel::LocalizationPanel() : EditorPanel("Localization", "localization_panel") {}

    bool LocalizationPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "LocalizationPanel initialized");
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "LocalizationPanel shutting down");
    }

    void LocalizationPanel::RenderToolbar()
    {
        if (!Spark::Ui::BeginPanelToolbar("##LocalizationToolbar"))
            return;

        // No CSV reader or writer exists for the string table yet; the buttons say so instead of
        // logging a request that nothing services.
        Spark::Ui::IconButton("ImportCsv", ICON_FA_FILE_IMPORT,
                              "Import CSV is unavailable: no CSV importer is implemented yet.", false, false);
        ImGui::SameLine();
        Spark::Ui::IconButton("ExportCsv", ICON_FA_FILE_EXPORT,
                              "Export CSV is unavailable: no CSV exporter is implemented yet.", false, false);
        Spark::Ui::ToolbarSeparator();
        ImGui::Checkbox("Show Missing Only", &m_showMissingOnly);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        Spark::Ui::SecondaryText("Char Warn");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Spark::Ui::Px(80));
        ImGui::DragInt("##CharWarn", &m_charLengthWarning, 1.0f, 10, 1000);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Warn when string exceeds this character count");
        Spark::Ui::EndPanelToolbar();
    }

    void LocalizationPanel::RenderLanguageSelector()
    {
        ImGui::AlignTextToFramePadding();
        Spark::Ui::SecondaryText(ICON_FA_GLOBE " Language");
        ImGui::SameLine();

        const char* langNames[] = {"English", "Spanish", "French", "German", "Japanese", "Chinese"};
        ImGui::SetNextItemWidth(Spark::Ui::Px(150));
        ImGui::Combo("##Language", &m_currentLanguage, langNames, IM_ARRAYSIZE(langNames));

        ImGui::SameLine();
        Spark::Ui::SecondaryText("Strings:");
        ImGui::SameLine();
        Spark::Ui::MonoText("%d", static_cast<int>(m_strings.size()));

        ImGui::SameLine();
        Spark::Ui::SearchField("##LocalizationFilter", m_filterText, sizeof(m_filterText), "Filter keys");
    }

    void LocalizationPanel::RenderCoverageMetrics()
    {
        if (m_strings.empty())
        {
            return;
        }

        Spark::Ui::SubHeader(ICON_FA_CHART_BAR " Coverage");
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
            std::snprintf(overlay, sizeof(overlay), "%s: %d/%d (%.0f%%)", lang.c_str(), translated, totalStrings,
                          coverage * 100.0f);

            const Spark::Ui::Role barRole = coverage >= 1.0f  ? Spark::Ui::Role::SuccessText
                                            : coverage > 0.5f ? Spark::Ui::Role::WarningText
                                                              : Spark::Ui::Role::ErrorText;
            {
                const Spark::Ui::ScopedRoleColor barColor(ImGuiCol_PlotHistogram, barRole);
                ImGui::ProgressBar(coverage, ImVec2(Spark::Ui::Px(250), 0.0f), overlay);
            }

            if (l % 3 != 2 && l < static_cast<int>(m_languages.size()) - 1)
                ImGui::SameLine();
        }
    }

    void LocalizationPanel::RenderStringTable()
    {
        ImGui::BeginChild("StringTable", ImVec2(0.0f, -Spark::Ui::Px(80)), true);

        if (m_strings.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_GLOBE, "No strings to analyze",
                                  "Add a key and its text with the form below to start the string table.");
            ImGui::EndChild();
            return;
        }

        // Columns: Key, Context, then one per language
        int columnCount = 2 + static_cast<int>(m_languages.size());
        if (Spark::Ui::BeginDataTable("L10NTable", columnCount))
        {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(180));
            ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(120));
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
                Spark::Ui::MonoText("%s", entry.key.c_str());

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
                    {
                        strncpy(buf, it->second.c_str(), sizeof(buf) - 1);
                        buf[sizeof(buf) - 1] = '\0';
                    }

                    char widgetId[32];
                    std::snprintf(widgetId, sizeof(widgetId), "##%s", lang.c_str());

                    // A missing translation gets the error fill and a "Missing" hint, so it is not
                    // signalled by colour alone.
                    const int length = static_cast<int>(strlen(buf));
                    const bool isMissing = length == 0;
                    const bool tooLong = !isMissing && length > m_charLengthWarning;
                    ImGui::SetNextItemWidth(tooLong ? -(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x)
                                                    : -1.0f);
                    {
                        const Spark::Ui::ScopedRoleColor fill(
                            ImGuiCol_FrameBg, isMissing ? Spark::Ui::Role::ErrorFill : Spark::Ui::Role::SurfaceSunken);
                        if (ImGui::InputTextWithHint(widgetId, isMissing ? "Missing" : "", buf, sizeof(buf),
                                                     ImGuiInputTextFlags_EnterReturnsTrue))
                            entry.translations[lang] = buf;
                    }

                    // Character length warning: warning glyph plus a tooltip that names it.
                    if (tooLong)
                    {
                        ImGui::SameLine();
                        {
                            const Spark::Ui::ScopedRoleColor warning(ImGuiCol_Text, Spark::Ui::Role::WarningText);
                            ImGui::TextUnformatted(Spark::Ui::StatusGlyph(Spark::Ui::Status::Warning));
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Warning: string exceeds %d characters (%d)", m_charLengthWarning,
                                              length);
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
        Spark::Ui::SubHeader("Add New Entry");
        ImGui::SetNextItemWidth(Spark::Ui::Px(180));
        ImGui::InputTextWithHint("##NewKey", "Key", m_newKey, sizeof(m_newKey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Spark::Ui::Px(200));
        ImGui::InputTextWithHint("##NewValue", "Value", m_newValue, sizeof(m_newValue));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Spark::Ui::Px(150));
        ImGui::InputTextWithHint("##NewContext", "Context", m_newContext, sizeof(m_newContext));
        ImGui::SameLine();

        const bool hasKey = m_newKey[0] != '\0';
        const Spark::Ui::ScopedDisabled disabled(!hasKey, "Enter a key first.");
        if (Spark::Ui::SecondaryButton(ICON_FA_PLUS " Add") && hasKey)
        {
            LocalizedString entry;
            entry.key = m_newKey;
            entry.context = m_newContext;
            if (m_currentLanguage < static_cast<int>(m_languages.size()))
                entry.translations[m_languages[m_currentLanguage]] = m_newValue;

            m_strings.push_back(entry);
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "LocalizationPanel: added string key '%s'", m_newKey);
            m_newKey[0] = '\0';
            m_newValue[0] = '\0';
            m_newContext[0] = '\0';
        }
    }

} // namespace SparkEditor
