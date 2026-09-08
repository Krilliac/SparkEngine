/**
 * @file SearchPanelSearch.cpp
 * @brief Live-source search, asset indexing, and result navigation for SearchPanel
 */

#include "SearchPanel.h"

#include "Core/Reflection.h"
#include "Engine/ECS/Components.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

namespace SparkEditor
{

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
