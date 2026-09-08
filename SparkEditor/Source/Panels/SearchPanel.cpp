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
        ImGui::SetNextItemWidth(-1);
        bool changed =
            ImGui::InputTextWithHint("##SearchInput", ICON_FA_SEARCH " Search entities, components, assets...",
                                     m_searchBuffer, sizeof(m_searchBuffer), ImGuiInputTextFlags_EnterReturnsTrue);

        if (changed || ImGui::IsItemEdited())
        {
            m_currentQuery = m_searchBuffer;
            if (m_currentQuery.empty())
            {
                m_searchCountdown = -1.0f;
                m_results.clear();
                m_selectedResult = -1;
            }
            else if (changed)
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
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) && m_selectedResult >= 0 &&
                m_selectedResult < static_cast<int>(m_results.size()))
            {
                NavigateToResult(m_results[static_cast<size_t>(m_selectedResult)]);
            }
        }
    }

    void SearchPanel::RenderFilterButtons()
    {
        auto FilterButton = [this](const char* label, SearchFilter filter)
        {
            bool isActive = (m_filter == filter);
            if (isActive)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::SmallButton(label))
            {
                m_filter = filter;
                if (!m_currentQuery.empty())
                {
                    PerformSearch();
                }
            }
            if (isActive)
            {
                ImGui::PopStyleColor();
            }
        };

        FilterButton("All", SearchFilter::All);
        ImGui::SameLine();
        FilterButton(ICON_FA_CUBE " Entities", SearchFilter::Entities);
        ImGui::SameLine();
        FilterButton(ICON_FA_COG " Components", SearchFilter::Components);
        ImGui::SameLine();
        FilterButton(ICON_FA_FOLDER " Assets", SearchFilter::Assets);
    }

    void SearchPanel::RenderResults()
    {
        ImGui::Text("%zu results for \"%s\"", m_results.size(), m_currentQuery.c_str());
        RenderSourceStatus();
        ImGui::Spacing();

        ImGui::BeginChild("SearchResults", ImVec2(0, 0), false);

        for (size_t i = 0; i < m_results.size(); ++i)
        {
            const auto& result = m_results[i];
            bool isSelected = (static_cast<int>(i) == m_selectedResult);

            ImGui::PushID(static_cast<int>(i));

            // Icon based on type
            const char* icon = ICON_FA_CUBE;
            ImVec4 iconColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            switch (result.type)
            {
            case SearchResultType::Entity:
                icon = ICON_FA_CUBE;
                iconColor = ImVec4(0.3f, 0.7f, 1.0f, 1.0f);
                break;
            case SearchResultType::Component:
                icon = ICON_FA_COG;
                iconColor = ImVec4(0.3f, 0.9f, 0.3f, 1.0f);
                break;
            case SearchResultType::Asset:
                icon = ICON_FA_FILE;
                iconColor = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
                break;
            }

            if (ImGui::Selectable(("##Result" + std::to_string(i)).c_str(), isSelected, 0, ImVec2(0, 36)))
            {
                m_selectedResult = static_cast<int>(i);
                NavigateToResult(result);
            }

            ImGui::SameLine(8);
            ImGui::TextColored(iconColor, "%s", icon);
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("%s", result.name.c_str());
            ImGui::TextDisabled("%s", result.description.c_str());
            ImGui::EndGroup();

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    void SearchPanel::RenderSourceStatus()
    {
        if (!m_world)
        {
            ImGui::TextDisabled("Preview - not connected: no World is wired in, so entity and");
            ImGui::TextDisabled("component results have no source.");
        }
        if (m_assetRoot.empty())
        {
            ImGui::TextDisabled("No project asset root is set, so asset results have no source.");
            return;
        }

        if (m_assetIndexValid)
        {
            ImGui::TextDisabled("Asset index: %zu files", m_assetIndex.size());
        }
        else
        {
            ImGui::TextDisabled("Asset index: not built yet");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rescan"))
        {
            RefreshAssetIndex();
            if (!m_currentQuery.empty())
            {
                PerformSearch();
            }
        }

        if (!m_assetScanError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_assetScanError.c_str());
        }
    }

    void SearchPanel::RenderRecentSearches()
    {
        RenderSourceStatus();
        if (m_recentSearches.empty())
        {
            ImGui::TextDisabled("Start typing to search across the scene and project");
            ImGui::Spacing();
            ImGui::TextDisabled("Tips:");
            ImGui::BulletText("Search for entity names");
            ImGui::BulletText("Search for component types (e.g., \"RigidBody\")");
            ImGui::BulletText("Search for asset files");
            return;
        }

        ImGui::Text(ICON_FA_SEARCH " Recent Searches");
        ImGui::Separator();

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
