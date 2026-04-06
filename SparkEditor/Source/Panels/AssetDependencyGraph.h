/**
 * @file AssetDependencyGraph.h
 * @brief Asset dependency tracking, reference graph, and audit tools
 * @author Spark Engine Team
 * @date 2026
 *
 * Builds and queries an asset reference graph for the entire project.
 * Provides tools for finding unused assets, circular dependencies,
 * size budgeting, and visualizing the dependency tree.
 *
 * ## Usage
 * @code
 *   auto& graph = SparkEditor::AssetDependencyGraph::GetInstance();
 *   graph.Initialize();
 *
 *   // Register asset and its dependencies
 *   graph.RegisterAsset("Materials/Metal.mat", AssetType::Material, 2048);
 *   graph.AddDependency("Materials/Metal.mat", "Textures/Metal_Albedo.png");
 *   graph.AddDependency("Materials/Metal.mat", "Textures/Metal_Normal.png");
 *
 *   // Build from scene
 *   graph.ScanDirectory("Assets/");
 *
 *   // Query
 *   auto refs = graph.GetReferencedBy("Textures/Metal_Albedo.png");
 *   auto deps = graph.GetDependencies("Materials/Metal.mat");
 *   auto unused = graph.FindUnusedAssets();
 *   auto circular = graph.FindCircularDependencies();
 *   auto budget = graph.GetSizeBudget("Level_01");
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SparkEditor
{

    /** @brief Asset type classification */
    enum class AssetType
    {
        Unknown,
        Texture,
        Material,
        Mesh,
        Animation,
        Audio,
        Scene,
        Prefab,
        Script,
        Shader,
        Font,
        Data,
        Video
    };

    /** @brief Information about a single asset in the graph */
    struct AssetNode
    {
        std::string path;                             ///< Relative asset path
        AssetType type = AssetType::Unknown;          ///< Asset type
        uint64_t sizeBytes = 0;                       ///< File size on disk
        std::unordered_set<std::string> dependencies; ///< Assets this asset depends on
        std::unordered_set<std::string> referencedBy; ///< Assets that depend on this asset
        bool isRootAsset = false;                     ///< Whether this is a scene/level root
    };

    /** @brief A circular dependency chain */
    struct CircularDependency
    {
        std::vector<std::string> chain; ///< A→B→C→A
    };

    /** @brief Size budget for a level/scene */
    struct SizeBudgetReport
    {
        std::string rootAsset;                                 ///< Level/scene name
        uint64_t totalSizeBytes = 0;                           ///< Total size of all transitive deps
        uint32_t assetCount = 0;                               ///< Number of unique assets
        std::vector<std::pair<std::string, uint64_t>> byType;  ///< Size breakdown by type
        std::vector<std::pair<std::string, uint64_t>> largest; ///< Top N largest assets
    };

    /** @brief Audit result for a full project scan */
    struct AuditReport
    {
        uint32_t totalAssets = 0;
        uint64_t totalSizeBytes = 0;
        uint32_t unusedAssets = 0;
        uint64_t unusedSizeBytes = 0;
        uint32_t circularDependencyCount = 0;
        std::vector<std::pair<AssetType, uint64_t>> sizeByType;
        std::vector<std::pair<AssetType, uint32_t>> countByType;
    };

    /**
     * @brief Asset dependency graph and audit system
     *
     * Tracks all assets and their inter-dependencies. Provides graph queries
     * for reference tracking, unused asset detection, circular dependency
     * detection, and size budgeting. Used by the Reference Viewer and
     * Asset Audit editor panels.
     *
     * Thread safety: Not thread-safe. Call from main thread only.
     */
    class AssetDependencyGraph
    {
      public:
        static AssetDependencyGraph& GetInstance()
        {
            static AssetDependencyGraph instance;
            return instance;
        }

        /** @brief Initialize the dependency graph */
        void Initialize()
        {
            m_nodes.clear();
            m_initialized = true;
        }

        /** @brief Shut down and clear all data */
        void Shutdown()
        {
            m_nodes.clear();
            m_initialized = false;
        }

        /**
         * @brief Register an asset in the graph
         * @param path Relative asset path
         * @param type Asset type
         * @param sizeBytes File size
         */
        void RegisterAsset(const std::string& path, AssetType type, uint64_t sizeBytes = 0)
        {
            if (!m_initialized || path.empty())
                return;
            auto& node = m_nodes[path];
            node.path = path;
            node.type = type;
            node.sizeBytes = sizeBytes;
        }

        /**
         * @brief Add a dependency edge: `from` depends on `to`
         * @param from Asset that has the dependency
         * @param to Asset that is depended upon
         */
        void AddDependency(const std::string& from, const std::string& to)
        {
            if (from.empty() || to.empty() || from == to)
                return;
            // Ensure both nodes exist
            m_nodes[from].path = from;
            m_nodes[to].path = to;
            m_nodes[from].dependencies.insert(to);
            m_nodes[to].referencedBy.insert(from);
        }

        /** @brief Remove a dependency edge */
        void RemoveDependency(const std::string& from, const std::string& to)
        {
            if (m_nodes.count(from))
                m_nodes[from].dependencies.erase(to);
            if (m_nodes.count(to))
                m_nodes[to].referencedBy.erase(from);
        }

        /** @brief Remove an asset and all its edges */
        void RemoveAsset(const std::string& path)
        {
            auto it = m_nodes.find(path);
            if (it == m_nodes.end())
                return;
            // Remove from all referencing nodes
            for (const auto& dep : it->second.dependencies)
            {
                if (m_nodes.count(dep))
                    m_nodes[dep].referencedBy.erase(path);
            }
            for (const auto& ref : it->second.referencedBy)
            {
                if (m_nodes.count(ref))
                    m_nodes[ref].dependencies.erase(path);
            }
            m_nodes.erase(it);
        }

        /** @brief Mark an asset as a root (scene/level) */
        void SetRootAsset(const std::string& path, bool isRoot = true)
        {
            if (m_nodes.count(path))
                m_nodes[path].isRootAsset = isRoot;
        }

        // ====================================================================
        // Queries
        // ====================================================================

        /** @brief Get direct dependencies of an asset */
        std::vector<std::string> GetDependencies(const std::string& path) const
        {
            auto it = m_nodes.find(path);
            if (it == m_nodes.end())
                return {};
            return {it->second.dependencies.begin(), it->second.dependencies.end()};
        }

        /** @brief Get assets that reference this asset */
        std::vector<std::string> GetReferencedBy(const std::string& path) const
        {
            auto it = m_nodes.find(path);
            if (it == m_nodes.end())
                return {};
            return {it->second.referencedBy.begin(), it->second.referencedBy.end()};
        }

        /** @brief Get all transitive dependencies (recursive) */
        std::vector<std::string> GetTransitiveDependencies(const std::string& path) const
        {
            std::unordered_set<std::string> visited;
            std::vector<std::string> result;
            std::queue<std::string> queue;
            queue.push(path);
            visited.insert(path);

            while (!queue.empty())
            {
                auto current = queue.front();
                queue.pop();
                auto it = m_nodes.find(current);
                if (it == m_nodes.end())
                    continue;
                for (const auto& dep : it->second.dependencies)
                {
                    if (visited.insert(dep).second)
                    {
                        result.push_back(dep);
                        queue.push(dep);
                    }
                }
            }
            return result;
        }

        /** @brief Find assets with no references (potentially unused) */
        std::vector<std::string> FindUnusedAssets() const
        {
            std::vector<std::string> unused;
            for (const auto& [path, node] : m_nodes)
            {
                if (node.referencedBy.empty() && !node.isRootAsset)
                    unused.push_back(path);
            }
            std::sort(unused.begin(), unused.end());
            return unused;
        }

        /** @brief Detect circular dependencies using DFS */
        std::vector<CircularDependency> FindCircularDependencies() const
        {
            std::vector<CircularDependency> cycles;
            std::unordered_set<std::string> globalVisited;

            for (const auto& [path, node] : m_nodes)
            {
                if (globalVisited.count(path))
                    continue;

                std::unordered_set<std::string> pathSet;
                std::vector<std::string> pathStack;
                DetectCycles(path, pathSet, pathStack, globalVisited, cycles);
            }
            return cycles;
        }

        /** @brief Generate a size budget report for a root asset */
        SizeBudgetReport GetSizeBudget(const std::string& rootPath) const
        {
            SizeBudgetReport report;
            report.rootAsset = rootPath;

            auto deps = GetTransitiveDependencies(rootPath);
            deps.push_back(rootPath); // Include the root itself

            std::unordered_map<AssetType, uint64_t> typeSize;

            for (const auto& dep : deps)
            {
                auto it = m_nodes.find(dep);
                if (it == m_nodes.end())
                    continue;
                report.totalSizeBytes += it->second.sizeBytes;
                report.assetCount++;
                typeSize[it->second.type] += it->second.sizeBytes;
                report.largest.push_back({dep, it->second.sizeBytes});
            }

            // Sort largest
            std::sort(report.largest.begin(), report.largest.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            if (report.largest.size() > 20)
                report.largest.resize(20);

            for (const auto& [type, size] : typeSize)
                report.byType.push_back({AssetTypeName(type), size});

            return report;
        }

        /** @brief Generate a full audit report */
        AuditReport GenerateAudit() const
        {
            AuditReport report;
            std::unordered_map<AssetType, uint64_t> sizeByType;
            std::unordered_map<AssetType, uint32_t> countByType;

            for (const auto& [path, node] : m_nodes)
            {
                report.totalAssets++;
                report.totalSizeBytes += node.sizeBytes;
                sizeByType[node.type] += node.sizeBytes;
                countByType[node.type]++;

                if (node.referencedBy.empty() && !node.isRootAsset)
                {
                    report.unusedAssets++;
                    report.unusedSizeBytes += node.sizeBytes;
                }
            }

            report.circularDependencyCount = static_cast<uint32_t>(FindCircularDependencies().size());

            for (const auto& [type, size] : sizeByType)
                report.sizeByType.push_back({type, size});
            for (const auto& [type, count] : countByType)
                report.countByType.push_back({type, count});

            return report;
        }

        /** @brief Get an asset node */
        const AssetNode* GetAsset(const std::string& path) const
        {
            auto it = m_nodes.find(path);
            return (it != m_nodes.end()) ? &it->second : nullptr;
        }

        /** @brief Get total asset count */
        size_t GetAssetCount() const { return m_nodes.size(); }

        /** @brief Get all asset paths */
        std::vector<std::string> GetAllAssetPaths() const
        {
            std::vector<std::string> paths;
            paths.reserve(m_nodes.size());
            for (const auto& [path, node] : m_nodes)
                paths.push_back(path);
            return paths;
        }

        /** @brief Clear all data */
        void Clear() { m_nodes.clear(); }

        /** @brief Get console-friendly status string */
        std::string Console_GetStatus() const
        {
            if (!m_initialized)
                return "[AssetGraph] Not initialized";
            uint64_t totalSize = 0;
            for (const auto& [path, node] : m_nodes)
                totalSize += node.sizeBytes;
            return "[AssetGraph] Assets: " + std::to_string(m_nodes.size()) + " | Total size: " + FormatSize(totalSize);
        }

      private:
        AssetDependencyGraph() = default;

        void DetectCycles(const std::string& node, std::unordered_set<std::string>& pathSet,
                          std::vector<std::string>& pathStack, std::unordered_set<std::string>& globalVisited,
                          std::vector<CircularDependency>& cycles) const
        {
            if (pathSet.count(node))
            {
                // Found a cycle — extract it
                CircularDependency cycle;
                auto startIt = std::find(pathStack.begin(), pathStack.end(), node);
                if (startIt != pathStack.end())
                {
                    for (auto it = startIt; it != pathStack.end(); ++it)
                        cycle.chain.push_back(*it);
                    cycle.chain.push_back(node);
                    cycles.push_back(std::move(cycle));
                }
                return;
            }

            if (globalVisited.count(node))
                return;

            pathSet.insert(node);
            pathStack.push_back(node);

            auto it = m_nodes.find(node);
            if (it != m_nodes.end())
            {
                for (const auto& dep : it->second.dependencies)
                    DetectCycles(dep, pathSet, pathStack, globalVisited, cycles);
            }

            pathStack.pop_back();
            pathSet.erase(node);
            globalVisited.insert(node);
        }

        static std::string AssetTypeName(AssetType type)
        {
            switch (type)
            {
            case AssetType::Texture:
                return "Texture";
            case AssetType::Material:
                return "Material";
            case AssetType::Mesh:
                return "Mesh";
            case AssetType::Animation:
                return "Animation";
            case AssetType::Audio:
                return "Audio";
            case AssetType::Scene:
                return "Scene";
            case AssetType::Prefab:
                return "Prefab";
            case AssetType::Script:
                return "Script";
            case AssetType::Shader:
                return "Shader";
            case AssetType::Font:
                return "Font";
            case AssetType::Data:
                return "Data";
            case AssetType::Video:
                return "Video";
            default:
                return "Unknown";
            }
        }

        static std::string FormatSize(uint64_t bytes)
        {
            if (bytes < 1024)
                return std::to_string(bytes) + " B";
            if (bytes < 1024 * 1024)
                return std::to_string(bytes / 1024) + " KB";
            if (bytes < 1024ULL * 1024 * 1024)
                return std::to_string(bytes / (1024 * 1024)) + " MB";
            return std::to_string(bytes / (1024ULL * 1024 * 1024)) + " GB";
        }

        bool m_initialized = false;
        std::unordered_map<std::string, AssetNode> m_nodes;
    };

} // namespace SparkEditor
