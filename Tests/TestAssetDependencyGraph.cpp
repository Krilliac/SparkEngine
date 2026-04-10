// TestAssetDependencyGraph.cpp - Tests for asset dependency graph logic
// Uses standalone reimplementation to avoid SparkEditor include path dependencies.
#include "TestFramework.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

    struct AssetInfo
    {
        std::string id;
        std::string type;
        uint64_t sizeBytes = 0;
    };

    class AssetDependencyGraph
    {
      public:
        void RegisterAsset(const AssetInfo& info)
        {
            m_assets[info.id] = info;
            m_deps[info.id];
            m_refs[info.id];
        }

        void AddDependency(const std::string& from, const std::string& to)
        {
            m_deps[from].insert(to);
            m_refs[to].insert(from);
        }

        std::vector<std::string> GetDependencies(const std::string& id) const
        {
            auto it = m_deps.find(id);
            return (it != m_deps.end()) ? std::vector<std::string>(it->second.begin(), it->second.end())
                                        : std::vector<std::string>{};
        }

        std::vector<std::string> GetReferencedBy(const std::string& id) const
        {
            auto it = m_refs.find(id);
            return (it != m_refs.end()) ? std::vector<std::string>(it->second.begin(), it->second.end())
                                        : std::vector<std::string>{};
        }

        std::vector<std::string> FindUnusedAssets() const
        {
            std::vector<std::string> unused;
            for (const auto& [id, refs] : m_refs)
                if (refs.empty() && m_assets.count(id))
                    unused.push_back(id);
            return unused;
        }

        std::vector<std::pair<std::string, std::string>> FindCircularDependencies() const
        {
            std::vector<std::pair<std::string, std::string>> cycles;
            for (const auto& [id, deps] : m_deps)
                for (const auto& dep : deps)
                {
                    auto it = m_deps.find(dep);
                    if (it != m_deps.end() && it->second.count(id))
                    {
                        auto a = std::min(id, dep), b = std::max(id, dep);
                        bool dup = false;
                        for (const auto& [ca, cb] : cycles)
                            if (ca == a && cb == b)
                            {
                                dup = true;
                                break;
                            }
                        if (!dup)
                            cycles.push_back({a, b});
                    }
                }
            return cycles;
        }

        std::vector<std::string> GetTransitiveDependencies(const std::string& id) const
        {
            std::unordered_set<std::string> visited;
            std::vector<std::string> result;
            Collect(id, visited, result);
            return result;
        }

        uint64_t GetTotalSize() const
        {
            uint64_t t = 0;
            for (const auto& [_, i] : m_assets)
                t += i.sizeBytes;
            return t;
        }

        std::vector<std::string> GetAssetsExceedingBudget(uint64_t budget) const
        {
            std::vector<std::string> over;
            for (const auto& [id, i] : m_assets)
                if (i.sizeBytes > budget)
                    over.push_back(id);
            return over;
        }

        size_t GetAssetCount() const { return m_assets.size(); }

      private:
        void Collect(const std::string& id, std::unordered_set<std::string>& v, std::vector<std::string>& r) const
        {
            auto it = m_deps.find(id);
            if (it == m_deps.end())
                return;
            for (const auto& d : it->second)
                if (v.insert(d).second)
                {
                    r.push_back(d);
                    Collect(d, v, r);
                }
        }

        std::unordered_map<std::string, AssetInfo> m_assets;
        std::unordered_map<std::string, std::unordered_set<std::string>> m_deps;
        std::unordered_map<std::string, std::unordered_set<std::string>> m_refs;
    };

} // anonymous namespace

TEST(AssetDependencyGraph_RegisterAsset)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"tex_wood", "texture", 1024});
    g.RegisterAsset({"mat_wood", "material", 256});
    EXPECT_EQ(static_cast<size_t>(2), g.GetAssetCount());
}

TEST(AssetDependencyGraph_AddDependency)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"mat", "material", 100});
    g.RegisterAsset({"tex", "texture", 500});
    g.AddDependency("mat", "tex");
    EXPECT_EQ(static_cast<size_t>(1), g.GetDependencies("mat").size());
}

TEST(AssetDependencyGraph_GetReferencedBy)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"mesh", "mesh", 100});
    g.RegisterAsset({"tex", "texture", 500});
    g.RegisterAsset({"mat", "material", 50});
    g.AddDependency("mesh", "tex");
    g.AddDependency("mat", "tex");
    EXPECT_EQ(static_cast<size_t>(2), g.GetReferencedBy("tex").size());
}

TEST(AssetDependencyGraph_FindUnusedAssets)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"used", "texture", 100});
    g.RegisterAsset({"unused", "texture", 200});
    g.RegisterAsset({"mat", "material", 50});
    g.AddDependency("mat", "used");
    auto unused = g.FindUnusedAssets();
    bool found = false;
    for (const auto& id : unused)
        if (id == "unused")
            found = true;
    EXPECT_TRUE(found);
}

TEST(AssetDependencyGraph_FindCircularDependencies)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"a", "mesh", 100});
    g.RegisterAsset({"b", "mesh", 100});
    g.AddDependency("a", "b");
    g.AddDependency("b", "a");
    EXPECT_EQ(static_cast<size_t>(1), g.FindCircularDependencies().size());
}

TEST(AssetDependencyGraph_NoCircularDependencies)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"a", "mesh", 100});
    g.RegisterAsset({"b", "texture", 200});
    g.AddDependency("a", "b");
    EXPECT_EQ(static_cast<size_t>(0), g.FindCircularDependencies().size());
}

TEST(AssetDependencyGraph_GetTransitiveDependencies)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"mesh", "mesh", 100});
    g.RegisterAsset({"mat", "material", 50});
    g.RegisterAsset({"tex", "texture", 200});
    g.AddDependency("mesh", "mat");
    g.AddDependency("mat", "tex");
    EXPECT_EQ(static_cast<size_t>(2), g.GetTransitiveDependencies("mesh").size());
}

TEST(AssetDependencyGraph_SizeBudget)
{
    AssetDependencyGraph g;
    g.RegisterAsset({"small", "texture", 100});
    g.RegisterAsset({"big", "texture", 5000});
    g.RegisterAsset({"medium", "texture", 1000});
    auto over = g.GetAssetsExceedingBudget(1000);
    EXPECT_EQ(static_cast<size_t>(1), over.size());
    EXPECT_EQ(static_cast<uint64_t>(6100), g.GetTotalSize());
}

// ============================================================================
// Real SparkEditor::AssetAuditGraph tests
// ----------------------------------------------------------------------------
// These exercise the actual editor-side singleton from
// SparkEditor/Source/Panels/AssetAuditGraph.h (formerly
// AssetDependencyGraph.h, renamed to resolve a latent name collision
// with AssetPipeline::AssetDependencyGraph). The class is header-only
// and has no ImGui dependency so the real implementation is directly
// includable into the test binary.
// ============================================================================

#include "Panels/AssetAuditGraph.h"

TEST(AssetAuditGraph_InitializeAndShutdown)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    EXPECT_EQ(g.GetAssetCount(), static_cast<size_t>(0));
    g.Shutdown();
}

TEST(AssetAuditGraph_RegisterAssetAndGet)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("a.png", SparkEditor::AuditAssetType::Texture, 1024);

    const auto* node = g.GetAsset("a.png");
    EXPECT_NE(node, nullptr);
    if (node)
    {
        EXPECT_EQ(node->path, std::string("a.png"));
        EXPECT_EQ(static_cast<int>(node->type), static_cast<int>(SparkEditor::AuditAssetType::Texture));
        EXPECT_EQ(node->sizeBytes, static_cast<uint64_t>(1024));
    }
    EXPECT_EQ(g.GetAsset("missing.png"), nullptr);
    g.Shutdown();
}

TEST(AssetAuditGraph_AddDependencyBidirectional)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("mat.mat", SparkEditor::AuditAssetType::Material, 500);
    g.RegisterAsset("tex.png", SparkEditor::AuditAssetType::Texture, 2048);

    g.AddDependency("mat.mat", "tex.png");

    auto deps = g.GetDependencies("mat.mat");
    EXPECT_EQ(deps.size(), static_cast<size_t>(1));
    EXPECT_EQ(deps[0], std::string("tex.png"));

    auto refs = g.GetReferencedBy("tex.png");
    EXPECT_EQ(refs.size(), static_cast<size_t>(1));
    EXPECT_EQ(refs[0], std::string("mat.mat"));

    g.Shutdown();
}

TEST(AssetAuditGraph_RemoveDependency)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("a", SparkEditor::AuditAssetType::Texture, 100);
    g.RegisterAsset("b", SparkEditor::AuditAssetType::Texture, 100);
    g.AddDependency("a", "b");
    EXPECT_EQ(g.GetDependencies("a").size(), static_cast<size_t>(1));

    g.RemoveDependency("a", "b");
    EXPECT_EQ(g.GetDependencies("a").size(), static_cast<size_t>(0));
    EXPECT_EQ(g.GetReferencedBy("b").size(), static_cast<size_t>(0));

    g.Shutdown();
}

TEST(AssetAuditGraph_TransitiveDependencies)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("scene", SparkEditor::AuditAssetType::Scene, 1000);
    g.RegisterAsset("mesh", SparkEditor::AuditAssetType::Mesh, 5000);
    g.RegisterAsset("mat", SparkEditor::AuditAssetType::Material, 500);
    g.RegisterAsset("tex", SparkEditor::AuditAssetType::Texture, 2048);

    g.AddDependency("scene", "mesh");
    g.AddDependency("mesh", "mat");
    g.AddDependency("mat", "tex");

    auto deps = g.GetTransitiveDependencies("scene");
    EXPECT_EQ(deps.size(), static_cast<size_t>(3));

    g.Shutdown();
}

TEST(AssetAuditGraph_FindUnusedAssets)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();

    g.RegisterAsset("used-by-mat", SparkEditor::AuditAssetType::Texture, 100);
    g.RegisterAsset("used-mat", SparkEditor::AuditAssetType::Material, 200);
    g.RegisterAsset("unused-tex", SparkEditor::AuditAssetType::Texture, 300);
    g.RegisterAsset("root-scene", SparkEditor::AuditAssetType::Scene, 1000);

    g.AddDependency("used-mat", "used-by-mat");
    g.AddDependency("root-scene", "used-mat");
    g.SetRootAsset("root-scene", true);

    auto unused = g.FindUnusedAssets();
    EXPECT_EQ(unused.size(), static_cast<size_t>(1));
    EXPECT_EQ(unused[0], std::string("unused-tex"));

    g.Shutdown();
}

TEST(AssetAuditGraph_FindCircularDependencies)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("a", SparkEditor::AuditAssetType::Mesh, 100);
    g.RegisterAsset("b", SparkEditor::AuditAssetType::Mesh, 100);
    g.RegisterAsset("c", SparkEditor::AuditAssetType::Mesh, 100);

    g.AddDependency("a", "b");
    g.AddDependency("b", "c");
    g.AddDependency("c", "a"); // cycle

    auto cycles = g.FindCircularDependencies();
    EXPECT_GT(cycles.size(), static_cast<size_t>(0));

    g.Shutdown();
}

TEST(AssetAuditGraph_RemoveAssetClearsEdges)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("parent", SparkEditor::AuditAssetType::Material, 100);
    g.RegisterAsset("child", SparkEditor::AuditAssetType::Texture, 200);
    g.AddDependency("parent", "child");

    g.RemoveAsset("child");
    EXPECT_EQ(g.GetAsset("child"), nullptr);
    // The 'parent' should no longer list 'child' in its dependencies.
    EXPECT_EQ(g.GetDependencies("parent").size(), static_cast<size_t>(0));

    g.Shutdown();
}

TEST(AssetAuditGraph_SizeBudgetReport)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("level.scene", SparkEditor::AuditAssetType::Scene, 500);
    g.RegisterAsset("mesh.fbx", SparkEditor::AuditAssetType::Mesh, 5000);
    g.RegisterAsset("tex.png", SparkEditor::AuditAssetType::Texture, 10000);
    g.AddDependency("level.scene", "mesh.fbx");
    g.AddDependency("mesh.fbx", "tex.png");

    auto report = g.GetSizeBudget("level.scene");
    EXPECT_EQ(report.rootAsset, std::string("level.scene"));
    EXPECT_EQ(report.totalSizeBytes, static_cast<uint64_t>(500 + 5000 + 10000));
    EXPECT_EQ(report.assetCount, static_cast<uint32_t>(3));

    g.Shutdown();
}

TEST(AssetAuditGraph_GenerateAuditAggregatesStats)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("a.png", SparkEditor::AuditAssetType::Texture, 1000);
    g.RegisterAsset("b.png", SparkEditor::AuditAssetType::Texture, 2000);
    g.RegisterAsset("m.mat", SparkEditor::AuditAssetType::Material, 500);
    g.AddDependency("m.mat", "a.png");
    // 'b.png' has no references and is not a root → unused.

    auto report = g.GenerateAudit();
    EXPECT_EQ(report.totalAssets, static_cast<uint32_t>(3));
    EXPECT_EQ(report.totalSizeBytes, static_cast<uint64_t>(3500));
    EXPECT_GE(report.unusedAssets, static_cast<uint32_t>(1));

    g.Shutdown();
}

TEST(AssetAuditGraph_ConsoleStatus)
{
    auto& g = SparkEditor::AssetAuditGraph::GetInstance();
    g.Initialize();
    g.RegisterAsset("x", SparkEditor::AuditAssetType::Texture, 42);
    const std::string s = g.Console_GetStatus();
    EXPECT_TRUE(s.find("[AssetGraph]") != std::string::npos);
    EXPECT_TRUE(s.find("Assets: 1") != std::string::npos);
    g.Shutdown();
}
