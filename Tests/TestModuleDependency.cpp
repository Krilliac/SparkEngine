// TestModuleDependency.cpp - Tests for module dependency resolution and topological sort
// Standalone implementations for CI testing (no DLL/shared library dependency)

#include "TestFramework.h"
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace TestModuleDep
{

    struct ModuleInfo
    {
        std::string name;
        int loadOrder = 1000;
        std::vector<std::string> dependencies;
    };

    /// Topological sort (Kahn's algorithm) matching ModuleManager::SortModules()
    std::vector<std::string> ResolveOrder(std::vector<ModuleInfo>& modules)
    {
        const size_t count = modules.size();

        std::unordered_map<std::string, size_t> nameToIndex;
        for (size_t i = 0; i < count; ++i)
            nameToIndex[modules[i].name] = i;

        std::vector<std::vector<size_t>> dependents(count);
        std::vector<int> inDegree(count, 0);

        for (size_t i = 0; i < count; ++i)
        {
            for (const auto& dep : modules[i].dependencies)
            {
                auto it = nameToIndex.find(dep);
                if (it != nameToIndex.end())
                {
                    dependents[it->second].push_back(i);
                    inDegree[i]++;
                }
            }
        }

        std::vector<std::string> sorted;
        sorted.reserve(count);

        auto getReady = [&]()
        {
            std::vector<size_t> ready;
            for (size_t i = 0; i < count; ++i)
            {
                if (inDegree[i] == 0)
                    ready.push_back(i);
            }
            std::stable_sort(ready.begin(), ready.end(),
                             [&](size_t a, size_t b) { return modules[a].loadOrder < modules[b].loadOrder; });
            return ready;
        };

        auto ready = getReady();
        while (!ready.empty())
        {
            for (size_t idx : ready)
            {
                sorted.push_back(modules[idx].name);
                inDegree[idx] = -1;
                for (size_t dep : dependents[idx])
                {
                    if (inDegree[dep] > 0)
                        inDegree[dep]--;
                }
            }
            ready = getReady();
        }

        return sorted;
    }

} // namespace TestModuleDep

using namespace TestModuleDep;

TEST(ModuleDeps_NoDeps_SortByLoadOrder)
{
    std::vector<ModuleInfo> modules = {{"ModuleC", 3000, {}}, {"ModuleA", 1000, {}}, {"ModuleB", 2000, {}}};

    auto order = ResolveOrder(modules);
    EXPECT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], std::string("ModuleA"));
    EXPECT_EQ(order[1], std::string("ModuleB"));
    EXPECT_EQ(order[2], std::string("ModuleC"));
}

TEST(ModuleDeps_DependencyOverridesLoadOrder)
{
    std::vector<ModuleInfo> modules = {{"ModuleA", 2000, {}}, {"ModuleB", 1000, {"ModuleA"}}};

    auto order = ResolveOrder(modules);
    EXPECT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], std::string("ModuleA"));
    EXPECT_EQ(order[1], std::string("ModuleB"));
}

TEST(ModuleDeps_TransitiveDeps)
{
    std::vector<ModuleInfo> modules = {
        {"ModuleC", 1000, {"ModuleB"}}, {"ModuleB", 1000, {"ModuleA"}}, {"ModuleA", 1000, {}}};

    auto order = ResolveOrder(modules);
    EXPECT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], std::string("ModuleA"));
    EXPECT_EQ(order[1], std::string("ModuleB"));
    EXPECT_EQ(order[2], std::string("ModuleC"));
}

TEST(ModuleDeps_DiamondDeps)
{
    std::vector<ModuleInfo> modules = {{"ModuleA", 1000, {}},
                                       {"ModuleB", 1000, {"ModuleA"}},
                                       {"ModuleC", 1000, {"ModuleA"}},
                                       {"ModuleD", 1000, {"ModuleB", "ModuleC"}}};

    auto order = ResolveOrder(modules);
    EXPECT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], std::string("ModuleA"));
    EXPECT_TRUE(order[1] == "ModuleB" || order[1] == "ModuleC");
    EXPECT_TRUE(order[2] == "ModuleB" || order[2] == "ModuleC");
    EXPECT_NE(order[1], order[2]);
    EXPECT_EQ(order[3], std::string("ModuleD"));
}

TEST(ModuleDeps_MissingDepSkipped)
{
    std::vector<ModuleInfo> modules = {{"ModuleA", 1000, {}}, {"ModuleB", 1000, {"Missing"}}};

    auto order = ResolveOrder(modules);
    EXPECT_EQ(order.size(), 2u);
}
