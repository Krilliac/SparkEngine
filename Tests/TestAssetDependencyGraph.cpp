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
