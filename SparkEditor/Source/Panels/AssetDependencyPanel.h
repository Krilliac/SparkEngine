/**
 * @file AssetDependencyPanel.h
 * @brief Asset dependency graph viewer for Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 *
 * Visualizes asset dependencies as an interactive graph, showing which
 * assets reference which other assets. Helps identify unused assets,
 * circular dependencies, and large dependency chains that affect
 * loading performance.
 */

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <cstdint>
#include <algorithm>

#include <imgui.h>

#include "../Core/EditorPanel.h"

namespace SparkEditor
{

    // =========================================================================
    // Asset dependency data model
    // =========================================================================

    /**
     * @brief Type of asset in the dependency graph
     */
    enum class AssetType
    {
        Texture,        ///< .png, .jpg, .dds, .tga
        Mesh,           ///< .fbx, .obj, .gltf
        Material,       ///< .sparkmat
        Shader,         ///< .hlsl, .glsl
        Audio,          ///< .wav, .ogg, .mp3
        Animation,      ///< .sparkanim
        Prefab,         ///< .sparkprefab
        Scene,          ///< .sparkscene
        Script,         ///< .as (AngelScript)
        Font,           ///< .ttf, .otf
        ParticleEffect, ///< .sparkfx
        Dialogue,       ///< .sparkdlg
        Config,         ///< .json, .xml, .ini
        Unknown         ///< Unrecognized file type
    };

    /**
     * @brief A single asset node in the dependency graph
     */
    struct AssetNode
    {
        std::string path; ///< Relative path from project root
        std::string name; ///< Display name (filename without extension)
        AssetType type = AssetType::Unknown;
        uint64_t fileSizeBytes = 0; ///< File size on disk

        /// Assets that this asset depends on (outgoing edges)
        std::vector<std::string> dependencies;

        /// Assets that depend on this asset (incoming edges — reverse lookup)
        std::vector<std::string> dependents;

        /// Editor graph layout position
        ImVec2 graphPosition{0, 0};
        bool isSelected = false;
        bool isHighlighted = false;

        /// Cached analysis results
        int depthFromRoot = -1; ///< Distance from scene root (-1 = not computed)
        bool isOrphan = false;  ///< No dependents (potentially unused)
        bool isInCycle = false; ///< Part of a circular dependency
    };

    /**
     * @brief Edge in the dependency graph
     */
    struct AssetEdge
    {
        std::string fromPath; ///< Source asset (the one that depends)
        std::string toPath;   ///< Target asset (the dependency)
        bool isHighlighted = false;
    };

    /**
     * @brief Analysis results for the dependency graph
     */
    struct DependencyAnalysis
    {
        size_t totalAssets = 0;
        size_t totalEdges = 0;
        size_t orphanedAssets = 0;                    ///< Assets with no dependents
        size_t circularDependencies = 0;              ///< Number of cycles detected
        std::vector<std::string> orphanPaths;         ///< Paths of orphaned assets
        std::vector<std::vector<std::string>> cycles; ///< Detected cycles
        size_t maxDependencyDepth = 0;                ///< Longest dependency chain
        std::string heaviestAsset;                    ///< Asset with most transitive deps
        uint64_t totalSizeBytes = 0;                  ///< Total size of all tracked assets
    };

    /**
     * @brief Filter settings for the graph view
     */
    struct DependencyFilter
    {
        bool showTextures = true;
        bool showMeshes = true;
        bool showMaterials = true;
        bool showShaders = true;
        bool showAudio = true;
        bool showAnimations = true;
        bool showPrefabs = true;
        bool showScenes = true;
        bool showScripts = true;
        bool showOther = true;
        bool showOrphansOnly = false;
        bool showCyclesOnly = false;
        std::string searchFilter;
        int maxDepth = -1; ///< Max depth to display (-1 = unlimited)
    };

    // =========================================================================
    // AssetDependencyPanel
    // =========================================================================

    /**
     * @brief Interactive asset dependency graph viewer
     *
     * Provides tools for understanding and managing asset relationships:
     * - Interactive graph view with nodes and edges
     * - Automatic layout (force-directed or hierarchical)
     * - Filter by asset type, name search, orphan/cycle status
     * - Dependency chain analysis (select an asset to highlight its deps)
     * - Orphan detection (assets with no references)
     * - Circular dependency detection and highlighting
     * - Asset size analysis and loading order optimization
     * - Export dependency report (CSV, JSON)
     */
    class AssetDependencyPanel : public EditorPanel
    {
      public:
        AssetDependencyPanel();
        ~AssetDependencyPanel() override;

        // EditorPanel interface
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        std::string GetTypeName() const override { return "AssetDependencyPanel"; }

        /**
         * @brief Scan the project directory and build the dependency graph
         * @param projectPath Root path to scan
         */
        void ScanProject(const std::string& projectPath);

        /**
         * @brief Run dependency analysis (orphans, cycles, depth)
         * @return Analysis results
         */
        DependencyAnalysis RunAnalysis();

        /**
         * @brief Select an asset and highlight its dependency chain
         * @param assetPath Path of the asset to select
         */
        void SelectAsset(const std::string& assetPath);

        /**
         * @brief Get the current filter settings
         */
        const DependencyFilter& GetFilter() const { return m_filter; }

        /**
         * @brief Set filter settings
         */
        void SetFilter(const DependencyFilter& filter);

        /**
         * @brief Export dependency report to file
         * @param filePath Output file path
         * @param format Export format ("csv" or "json")
         * @return true if export succeeded
         */
        bool ExportReport(const std::string& filePath, const std::string& format = "json");

        /**
         * @brief Get the number of tracked assets
         */
        size_t GetAssetCount() const { return m_nodes.size(); }

      private:
        // Render sub-sections
        void RenderToolbar();
        void RenderGraphView();
        void RenderAssetNode(AssetNode& node);
        void RenderEdges();
        void RenderFilterPanel();
        void RenderInspector();
        void RenderAnalysisResults();
        void RenderAssetList();

        // Graph helpers
        void BuildGraph();
        void LayoutGraph();
        void ForceDirectedLayout(int iterations);
        void HighlightDependencyChain(const std::string& assetPath);
        void ClearHighlights();
        void DetectCycles();
        void ComputeDepths();

        // Canvas helpers
        ImVec2 ScreenToGraph(ImVec2 screenPos) const;
        ImVec2 GraphToScreen(ImVec2 graphPos) const;
        void HandleGraphInput();
        ImVec4 GetAssetTypeColor(AssetType type) const;
        const char* GetAssetTypeName(AssetType type) const;
        AssetType ClassifyAsset(const std::string& path) const;
        bool PassesFilter(const AssetNode& node) const;

      private:
        std::unordered_map<std::string, AssetNode> m_nodes;
        std::vector<AssetEdge> m_edges;
        DependencyFilter m_filter;
        DependencyAnalysis m_lastAnalysis;
        bool m_analysisStale = true;

        // Graph view state
        ImVec2 m_graphOffset{0, 0};
        float m_graphZoom = 1.0f;
        bool m_isDraggingGraph = false;
        ImVec2 m_graphDragStart{0, 0};

        // Selection state
        std::string m_selectedAssetPath;
        std::string m_hoveredAssetPath;
        bool m_isDraggingNode = false;

        // UI state
        bool m_showFilterPanel = true;
        bool m_showInspector = true;
        bool m_showAnalysis = false;
        bool m_showListView = false;
        bool m_needsRelayout = false;

        // Layout parameters
        float m_layoutRepulsion = 500.0f;
        float m_layoutAttraction = 0.01f;
        float m_layoutDamping = 0.9f;
    };

} // namespace SparkEditor
