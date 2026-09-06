/**
 * @file SearchPanel.cpp
 * @brief Implementation of the search panel
 * @author Spark Engine Team
 * @date 2025
 */

#include "SearchPanel.h"
#include "../Core/EditorIcons.h"
#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <utility>

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

    void SearchPanel::Search(const std::string& query)
    {
        m_currentQuery = query;
        PerformSearch();
    }

    void SearchPanel::RefreshAssetIndex()
    {
        namespace fs = std::filesystem;

        m_assetIndex.clear();
        m_assetScanError.clear();
        m_assetIndexValid = true;

        if (m_assetRoot.empty())
        {
            return;
        }

        std::error_code ec;
        if (!fs::exists(m_assetRoot, ec) || ec)
        {
            m_assetScanError = "Asset root does not exist: " + m_assetRoot;
            return;
        }

        // An explicit iterator with the error_code increment. The range-for form uses the THROWING
        // operator++, so an I/O error mid-walk escaped as filesystem_error out of the render path,
        // while the `if (ec) break;` inside the loop body only ever re-read the constructor's status.
        const fs::directory_options options = fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator it(m_assetRoot, options, ec);
        if (ec)
        {
            m_assetScanError = "Failed to open asset root '" + m_assetRoot + "': " + ec.message();
            return;
        }

        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code entryEc;
            if (!it->is_regular_file(entryEc) || entryEc)
                continue;

            m_assetIndex.push_back(it->path().string());
        }

        // A failed increment leaves the iterator equal to end, so the error must be read after the
        // loop; checking it only inside the body reported nothing.
        if (ec)
        {
            m_assetScanError = "Asset scan stopped: " + ec.message();
        }

        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Indexed %zu asset files under '%s'", m_assetIndex.size(),
                        m_assetRoot.c_str());
    }

    void SearchPanel::PerformSearch()
    {
        m_results.clear();
        m_selectedResult = -1;

        if (m_currentQuery.empty())
        {
            return;
        }
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Performing search: '%s'", m_currentQuery.c_str());

        std::string lowerQuery = m_currentQuery;
        std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(), ::tolower);

        const bool wantEntities = (m_filter == SearchFilter::All || m_filter == SearchFilter::Entities);
        const bool wantComponents = (m_filter == SearchFilter::All || m_filter == SearchFilter::Components);

        // Entities and components come from the live document World.
        if (m_world && (wantEntities || wantComponents))
        {
            entt::registry& registry = m_world->GetRegistry();

            // Only component types whose NAME matches can ever produce a result, so score the names
            // once here instead of scoring (and probing) every registered type on every entity.
            std::vector<std::pair<std::string, float>> matchingTypes;
            if (wantComponents)
            {
                for (const std::string& typeName : Spark::ComponentFactory::Get().GetRegisteredNames())
                {
                    const float score = CalculateRelevance(typeName, lowerQuery);
                    if (score > 0.0f)
                    {
                        matchingTypes.emplace_back(typeName, score);
                    }
                }
            }

            for (auto&& [entity] : registry.storage<entt::entity>().each())
            {
                const ::NameComponent* nameComponent = m_world->GetComponent<::NameComponent>(entity);
                const std::string entityName =
                    (nameComponent && !nameComponent->name.empty()) ? nameComponent->name : std::string("Entity");

                if (wantEntities)
                {
                    const float score = CalculateRelevance(entityName, lowerQuery);
                    if (score > 0.0f)
                    {
                        SearchResult result;
                        result.type = SearchResultType::Entity;
                        result.name = entityName;
                        result.description = "Entity " + std::to_string(static_cast<uint32_t>(entity));
                        result.entityId = static_cast<uint64_t>(static_cast<uint32_t>(entity));
                        result.relevanceScore = score;
                        m_results.push_back(std::move(result));
                    }
                }

                for (const auto& [typeName, score] : matchingTypes)
                {
                    if (!Spark::ComponentFactory::Get().HasComponent(typeName, m_world, static_cast<uint32_t>(entity)))
                    {
                        continue;
                    }

                    SearchResult result;
                    result.type = SearchResultType::Component;
                    result.name = typeName;
                    result.description = "on " + entityName;
                    result.entityId = static_cast<uint64_t>(static_cast<uint32_t>(entity));
                    result.relevanceScore = score;
                    m_results.push_back(std::move(result));
                }
            }
        }

        // Assets come from the cached index, which is walked on demand rather than per keystroke.
        if ((m_filter == SearchFilter::All || m_filter == SearchFilter::Assets) && !m_assetRoot.empty())
        {
            if (!m_assetIndexValid)
            {
                RefreshAssetIndex();
            }

            for (const std::string& assetPath : m_assetIndex)
            {
                const std::string fileName = std::filesystem::path(assetPath).filename().string();
                const float score = CalculateRelevance(fileName, lowerQuery);
                if (score <= 0.0f)
                    continue;

                SearchResult result;
                result.type = SearchResultType::Asset;
                result.name = fileName;
                result.path = assetPath;
                result.description = assetPath;
                result.relevanceScore = score;
                m_results.push_back(std::move(result));
            }
        }

        // Sort by relevance
        std::sort(m_results.begin(), m_results.end(),
                  [](const SearchResult& a, const SearchResult& b) { return a.relevanceScore > b.relevanceScore; });

        // Limit results
        if (m_results.size() > 50)
        {
            m_results.resize(50);
        }
    }

    float SearchPanel::CalculateRelevance(const std::string& text, const std::string& query) const
    {
        std::string lowerText = text;
        std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);

        // Exact match
        if (lowerText == query)
        {
            return 1.0f;
        }

        // Starts with
        if (lowerText.find(query) == 0)
        {
            return 0.9f;
        }

        // Contains
        if (lowerText.contains(query))
        {
            return 0.7f;
        }

        // Fuzzy: check if all characters in query appear in order in text
        size_t qi = 0;
        for (size_t ti = 0; ti < lowerText.size() && qi < query.size(); ++ti)
        {
            if (lowerText[ti] == query[qi])
            {
                ++qi;
            }
        }
        if (qi == query.size())
        {
            return 0.3f + 0.2f * (static_cast<float>(query.size()) / static_cast<float>(lowerText.size()));
        }

        return 0.0f;
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

    void SearchPanel::NavigateToResult(const SearchResult& result)
    {
        if (result.type == SearchResultType::Asset)
        {
            // Asset results carry the real path; the editor has no asset-browser
            // navigation entry point, so the path is logged rather than pretending
            // to reveal it in another panel.
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "SearchPanel: asset '%s'", result.path.c_str());
            return;
        }

        if (!m_selectionHandler || !m_world)
        {
            return;
        }

        const auto entityId = static_cast<uint32_t>(result.entityId);
        if (m_world->GetRegistry().valid(static_cast<::EntityID>(entityId)))
        {
            m_selectionHandler(entityId);
        }
    }

} // namespace SparkEditor
