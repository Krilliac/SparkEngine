/**
 * @file SearchPanel.h
 * @brief Search panel for finding entities, components, and assets
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a global search interface with fuzzy matching across
 * entities, components, and assets in the current scene and project.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Type of search result
     */
    enum class SearchResultType
    {
        Entity,
        Component,
        Asset
    };

    /**
     * @brief A single search result entry
     */
    struct SearchResult
    {
        SearchResultType type;       ///< Result type
        std::string name;            ///< Display name
        std::string description;     ///< Additional context
        std::string path;            ///< File path (for assets) or hierarchy path (for entities)
        uint64_t entityId = 0;       ///< Entity ID (for entity/component results)
        float relevanceScore = 0.0f; ///< Match quality score (higher = better)
    };

    /**
     * @brief Search filter type selection
     */
    enum class SearchFilter
    {
        All,
        Entities,
        Components,
        Assets
    };

    /**
     * @brief Global search panel for the editor
     *
     * Provides fuzzy search across entities, components, and assets
     * with configurable filters, result navigation, and search history.
     */
    class SearchPanel : public EditorPanel
    {
      public:
        SearchPanel();
        ~SearchPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "SearchPanel"; }

      private:
        void PerformSearch();
        void RenderSearchBar();
        void RenderFilterButtons();
        void RenderResults();
        void RenderRecentSearches();
        void AddToRecentSearches(const std::string& query);
        void NavigateToResult(const SearchResult& result);
        float CalculateRelevance(const std::string& text, const std::string& query) const;

        // Search state
        char m_searchBuffer[256] = {};
        std::string m_currentQuery;
        SearchFilter m_filter = SearchFilter::All;
        std::vector<SearchResult> m_results;
        int m_selectedResult = -1;

        // Recent searches
        static constexpr size_t MAX_RECENT_SEARCHES = 20;
        std::vector<std::string> m_recentSearches;
        bool m_showRecent = false;

        // Sample data for search demonstration
        struct SampleEntity
        {
            std::string name;
            uint64_t id;
            std::vector<std::string> components;
            std::string hierarchyPath;
        };
        std::vector<SampleEntity> m_sampleEntities;

        struct SampleAsset
        {
            std::string name;
            std::string type;
            std::string path;
        };
        std::vector<SampleAsset> m_sampleAssets;

        void InitializeSampleData();
    };

} // namespace SparkEditor
