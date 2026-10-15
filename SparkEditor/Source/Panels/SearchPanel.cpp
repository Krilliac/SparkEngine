/**
 * @file SearchPanel.cpp
 * @brief Implementation of the search panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SearchPanel.h"
#include "../Core/EditorIcons.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <imgui.h>
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    SearchPanel::SearchPanel() : EditorPanel("Search", "Search") {}

    bool SearchPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Search panel");
        m_isInitialized = true;
        return true;
    }

    void SearchPanel::Update(float deltaTime)
    {
        // Typing schedules a search instead of running one: PerformSearch() filters a cached asset
        // index, but the index rebuild and the per-entity component probe are still far too heavy
        // to run once per character.
        if (m_searchCountdown < 0.0f)
        {
            return;
        }

        m_searchCountdown -= deltaTime;
        if (m_searchCountdown <= 0.0f)
        {
            m_searchCountdown = -1.0f;
            PerformSearch();
        }
    }

    void SearchPanel::Render()
    {
        if (!m_isVisible)
        {
            return;
        }

        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        RenderSearchBar();
        RenderFilterButtons();
        ImGui::Separator();

        if (m_currentQuery.empty())
        {
            RenderRecentSearches();
        }
        else
        {
            RenderResults();
        }

        EndPanel();
    }

    void SearchPanel::Shutdown() {}

    void SearchPanel::RenderSearchBar()
    {
        const bool edited = Spark::Ui::SearchField("##SearchInput", m_searchBuffer, sizeof(m_searchBuffer),
                                                   "Search entities, components, assets...");
        // Enter ends the edit: search immediately instead of waiting for the debounce window.
        const bool submitted = ImGui::IsItemDeactivated() &&
                               (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));

        if (edited || submitted)
        {
            m_currentQuery = m_searchBuffer;
            if (m_currentQuery.empty())
            {
                m_searchCountdown = -1.0f;
                m_results.clear();
                m_selectedResult = -1;
            }
            else if (submitted)
            {
                // Enter: search now.
                m_searchCountdown = -1.0f;
                PerformSearch();
                AddToRecentSearches(m_currentQuery);
            }
            else
            {
                // Still typing: restart the debounce window, Update() runs the search.
                m_searchCountdown = kSearchDebounceSeconds;
            }
        }

        // Navigate results with arrow keys
        if (ImGui::IsItemActive())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !m_results.empty())
            {
                m_selectedResult = std::min(m_selectedResult + 1, static_cast<int>(m_results.size()) - 1);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
            {
                m_selectedResult = std::max(m_selectedResult - 1, 0);
            }
        }
        if (submitted && m_selectedResult >= 0 && m_selectedResult < static_cast<int>(m_results.size()))
        {
            NavigateToResult(m_results[static_cast<size_t>(m_selectedResult)]);
        }
    }

    void SearchPanel::RenderFilterButtons()
    {
        // Label order matches the SearchFilter enumerators.
        static constexpr const char* kFilterLabels[] = {"All", ICON_FA_CUBE " Entities", ICON_FA_COG " Components",
                                                        ICON_FA_FOLDER " Assets"};
        int current = static_cast<int>(m_filter);
        if (Spark::Ui::SegmentedControl("##SearchFilter", kFilterLabels, static_cast<int>(IM_ARRAYSIZE(kFilterLabels)),
                                        &current))
        {
            m_filter = static_cast<SearchFilter>(current);
            if (!m_currentQuery.empty())
            {
                PerformSearch();
            }
        }
    }

    void SearchPanel::RenderResults()
    {
        Spark::Ui::SecondaryText("%zu results for \"%s\"", m_results.size(), m_currentQuery.c_str());
        RenderSourceStatus();
        ImGui::Spacing();

        if (m_results.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_SEARCH, "No matches",
                                  "Try a different name, or switch the filter back to All.");
            return;
        }

        ImGui::BeginChild("SearchResults", ImVec2(0, 0), false);

        for (size_t i = 0; i < m_results.size(); ++i)
        {
            const auto& result = m_results[i];
            bool isSelected = (static_cast<int>(i) == m_selectedResult);

            ImGui::PushID(static_cast<int>(i));

            // The icon shape identifies the result type; the category colour only reinforces it.
            const char* icon = ICON_FA_CUBE;
            int category = 0;
            switch (result.type)
            {
            case SearchResultType::Entity:
                icon = ICON_FA_CUBE;
                category = 0;
                break;
            case SearchResultType::Component:
                icon = ICON_FA_COG;
                category = 1;
                break;
            case SearchResultType::Asset:
                icon = ICON_FA_FILE;
                category = 2;
                break;
            }

            if (ImGui::Selectable(("##Result" + std::to_string(i)).c_str(), isSelected, 0,
                                  ImVec2(0.0f, Spark::Ui::Px(36))))
            {
                m_selectedResult = static_cast<int>(i);
                NavigateToResult(result);
            }

            ImGui::SameLine(Spark::Ui::Px(8));
            ImGui::PushStyleColor(ImGuiCol_Text, Spark::Ui::CategoryColor(category));
            ImGui::TextUnformatted(icon);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(result.name.c_str());
            Spark::Ui::SecondaryText("%s", result.description.c_str());
            ImGui::EndGroup();

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    void SearchPanel::RenderSourceStatus()
    {
        if (!m_world)
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Info,
                                  "Preview - not connected: no World is wired in, so entity and");
            Spark::Ui::SecondaryText("component results have no source.");
        }
        if (m_assetRoot.empty())
        {
            Spark::Ui::SecondaryText("No project asset root is set, so asset results have no source.");
            return;
        }

        if (m_assetIndexValid)
        {
            Spark::Ui::SecondaryText("Asset index: %zu files", m_assetIndex.size());
        }
        else
        {
            Spark::Ui::SecondaryText("Asset index: not built yet");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_REFRESH " Rescan"))
        {
            RefreshAssetIndex();
            if (!m_currentQuery.empty())
            {
                PerformSearch();
            }
        }

        if (!m_assetScanError.empty())
        {
            Spark::Ui::StatusText(Spark::Ui::Status::Error, "%s", m_assetScanError.c_str());
        }
    }

    void SearchPanel::RenderRecentSearches()
    {
        RenderSourceStatus();
        if (m_recentSearches.empty())
        {
            Spark::Ui::EmptyState(ICON_FA_SEARCH, "Start typing to search across the scene and project",
                                  "Search for entity names, component types (e.g., \"RigidBody\") or asset files.");
            return;
        }

        Spark::Ui::SubHeader(ICON_FA_HISTORY " Recent Searches");

        for (size_t i = 0; i < m_recentSearches.size(); ++i)
        {
            if (ImGui::Selectable(m_recentSearches[i].c_str()))
            {
                std::string query = m_recentSearches[i];
                std::copy(query.begin(), query.begin() + std::min(query.size(), sizeof(m_searchBuffer) - 1),
                          m_searchBuffer);
                m_searchBuffer[std::min(query.size(), sizeof(m_searchBuffer) - 1)] = '\0';
                m_currentQuery = query;
                PerformSearch();
            }
        }

        ImGui::Spacing();
        if (ImGui::SmallButton("Clear History"))
        {
            m_recentSearches.clear();
        }
    }

    void SearchPanel::AddToRecentSearches(const std::string& query)
    {
        // Remove if already exists
        auto it = std::find(m_recentSearches.begin(), m_recentSearches.end(), query);
        if (it != m_recentSearches.end())
        {
            m_recentSearches.erase(it);
        }

        // Add to front
        m_recentSearches.insert(m_recentSearches.begin(), query);

        // Trim
        if (m_recentSearches.size() > MAX_RECENT_SEARCHES)
        {
            m_recentSearches.resize(MAX_RECENT_SEARCHES);
        }
    }


} // namespace SparkEditor
