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
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class World;

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

        /// @brief Point the panel at the document World whose entities it searches.
        void SetWorld(::World* world) { m_world = world; }

        /// @brief Handler invoked with the entity id when an entity/component result is opened.
        void SetSelectionHandler(std::function<void(uint32_t)> handler) { m_selectionHandler = std::move(handler); }

        /// @brief Directory searched for asset results (empty = no project asset root).
        /// Changing the root invalidates the cached asset index; the next search rebuilds it.
        void SetAssetRoot(const std::string& assetRoot)
        {
            m_assetRoot = assetRoot;
            InvalidateAssetIndex();
        }

        /// @brief Drop the cached asset file list so the next search re-walks the asset root.
        void InvalidateAssetIndex()
        {
            m_assetIndex.clear();
            m_assetIndexValid = false;
            m_assetScanError.clear();
        }

        /// @brief Walk the asset root once and cache every regular file found.
        /// The walk is deliberately NOT per-keystroke: a project asset tree is far too large to
        /// re-enumerate inside a UI frame.
        void RefreshAssetIndex();

        /// @brief Number of files in the cached asset index (0 when it has not been built).
        size_t GetIndexedAssetCount() const { return m_assetIndex.size(); }

        /// @brief Message describing the last asset-scan failure, empty when the scan was clean.
        const std::string& GetAssetScanError() const { return m_assetScanError; }

        /// @brief Whether a World is wired in; without one entity search has no source.
        bool IsWorldConnected() const { return m_world != nullptr; }

        /// @brief Run the search for @p query against the live sources.
        void Search(const std::string& query);

        /// @brief Results produced by the last Search().
        const std::vector<SearchResult>& GetResults() const { return m_results; }

        /**
         * @brief Act on a result: select the entity, or report the asset path.
         *
         * Entity and component results publish the entity id to the selection
         * handler; without a handler or World nothing is selected and nothing is
         * reported as having been opened.
         */
        void NavigateToResult(const SearchResult& result);

      private:
        void PerformSearch();
        void RenderSearchBar();
        void RenderFilterButtons();
        void RenderResults();
        void RenderSourceStatus();
        void RenderRecentSearches();
        void AddToRecentSearches(const std::string& query);
        float CalculateRelevance(const std::string& text, const std::string& query) const;

        /// @brief Seconds of typing inactivity before an edited query is searched.
        static constexpr float kSearchDebounceSeconds = 0.25f;

        // Search state
        char m_searchBuffer[256] = {};
        std::string m_currentQuery;
        /// Countdown until the pending query is searched; negative means nothing is pending.
        float m_searchCountdown = -1.0f;
        SearchFilter m_filter = SearchFilter::All;
        std::vector<SearchResult> m_results;
        int m_selectedResult = -1;

        // Recent searches
        static constexpr size_t MAX_RECENT_SEARCHES = 20;
        std::vector<std::string> m_recentSearches;
        bool m_showRecent = false;

        // Live sources (all non-owning / injected by the editor shell).
        ::World* m_world = nullptr;
        std::function<void(uint32_t)> m_selectionHandler;
        std::string m_assetRoot;

        // Cached asset index; rebuilt on demand rather than on every keystroke.
        std::vector<std::string> m_assetIndex;
        bool m_assetIndexValid = false;
        std::string m_assetScanError;
    };

} // namespace SparkEditor
