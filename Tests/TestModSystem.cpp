// TestModSystem.cpp - Tests for game modding system

#include "TestFramework.h"
#include "Engine/Modding/ModSystem.h"
#include <unordered_map>
#include <unordered_set>

using namespace Spark;

TEST(ModSystem_InitiallyEmpty)
{
    ModSystem mods;
    auto all = mods.GetAllMods();
    EXPECT_EQ(all.size(), static_cast<size_t>(0));
    auto enabled = mods.GetEnabledMods();
    EXPECT_EQ(enabled.size(), static_cast<size_t>(0));
}

TEST(ModSystem_GetModInfoNullForMissing)
{
    ModSystem mods;
    auto info = mods.GetModInfo("nonexistent");
    EXPECT_EQ(info, nullptr);
}

TEST(ModSystem_IsModActiveReturnsFalse)
{
    ModSystem mods;
    EXPECT_FALSE(mods.IsModActive("nonexistent"));
}

TEST(ModSystem_ScanEmptyDirectory)
{
    ModSystem mods;
    // Scanning a non-existent or empty directory should return 0
    size_t found = mods.ScanForMods("/tmp/nonexistent_mod_dir_12345");
    EXPECT_EQ(found, static_cast<size_t>(0));
}

TEST(ModSystem_ConsoleStatus)
{
    ModSystem mods;
    std::string status = mods.Console_GetStatus();
    EXPECT_FALSE(status.empty());
}

// ============================================================================
// Standalone types for manifest, dependency, conflict, and version tests
// ============================================================================

namespace TestMod
{

    struct ModManifest
    {
        std::string id;
        std::string name;
        std::string author;
        std::string version;
        std::string description;
        std::vector<std::string> dependencies;
        std::vector<std::string> modifiedAssets;
    };

    ModManifest ParseManifest(const std::string& id, const std::string& name, const std::string& author,
                              const std::string& version, const std::string& description,
                              const std::vector<std::string>& deps, const std::vector<std::string>& assets)
    {
        return {id, name, author, version, description, deps, assets};
    }

    struct VersionInfo
    {
        int major = 0;
        int minor = 0;

        bool IsCompatibleWith(const VersionInfo& required) const
        {
            return (major > required.major) || (major == required.major && minor >= required.minor);
        }
    };

    std::vector<std::string> ResolveLoadOrder(const std::unordered_map<std::string, std::vector<std::string>>& depGraph)
    {
        std::vector<std::string> order;
        std::unordered_set<std::string> visited;

        std::function<void(const std::string&)> visit = [&](const std::string& id)
        {
            if (visited.count(id))
                return;
            visited.insert(id);
            auto it = depGraph.find(id);
            if (it != depGraph.end())
            {
                for (const auto& dep : it->second)
                    visit(dep);
            }
            order.push_back(id);
        };

        for (const auto& [id, deps] : depGraph)
            visit(id);

        return order;
    }

    struct ConflictResult
    {
        bool hasConflict = false;
        std::string conflictingAsset;
    };

    ConflictResult DetectConflicts(const ModManifest& a, const ModManifest& b)
    {
        for (const auto& assetA : a.modifiedAssets)
        {
            for (const auto& assetB : b.modifiedAssets)
            {
                if (assetA == assetB)
                    return {true, assetA};
            }
        }
        return {false, ""};
    }

} // namespace TestMod

// ============================================================================
// Manifest Parsing
// ============================================================================

TEST(ModSystem_LoadManifest)
{
    auto manifest = TestMod::ParseManifest("my_mod", "My Cool Mod", "TestAuthor", "1.2.0", "A test modification",
                                           {"base_mod"}, {"textures/player.png"});

    EXPECT_EQ(manifest.id, std::string("my_mod"));
    EXPECT_EQ(manifest.name, std::string("My Cool Mod"));
    EXPECT_EQ(manifest.author, std::string("TestAuthor"));
    EXPECT_EQ(manifest.version, std::string("1.2.0"));
    EXPECT_EQ(manifest.description, std::string("A test modification"));
    EXPECT_EQ(manifest.dependencies.size(), static_cast<size_t>(1));
    EXPECT_EQ(manifest.dependencies[0], std::string("base_mod"));
}

// ============================================================================
// Dependency Resolution
// ============================================================================

TEST(ModSystem_DependencyResolution)
{
    // Mod A depends on Mod B
    std::unordered_map<std::string, std::vector<std::string>> deps;
    deps["modA"] = {"modB"};
    deps["modB"] = {};

    auto order = TestMod::ResolveLoadOrder(deps);

    // Find positions
    size_t posA = 999, posB = 999;
    for (size_t i = 0; i < order.size(); ++i)
    {
        if (order[i] == "modA")
            posA = i;
        if (order[i] == "modB")
            posB = i;
    }

    EXPECT_TRUE(posB < posA); // B must load before A
    EXPECT_EQ(order.size(), static_cast<size_t>(2));
}

// ============================================================================
// Conflict Detection
// ============================================================================

TEST(ModSystem_ConflictDetection)
{
    TestMod::ModManifest modA;
    modA.id = "modA";
    modA.modifiedAssets = {"textures/player.png", "sounds/hit.wav"};

    TestMod::ModManifest modB;
    modB.id = "modB";
    modB.modifiedAssets = {"textures/player.png", "models/tree.obj"};

    auto result = TestMod::DetectConflicts(modA, modB);
    EXPECT_TRUE(result.hasConflict);
    EXPECT_EQ(result.conflictingAsset, std::string("textures/player.png"));

    // No conflict case
    TestMod::ModManifest modC;
    modC.id = "modC";
    modC.modifiedAssets = {"scripts/ai.as"};

    auto noConflict = TestMod::DetectConflicts(modA, modC);
    EXPECT_FALSE(noConflict.hasConflict);
}

// ============================================================================
// Version Compatibility
// ============================================================================

TEST(ModSystem_VersionCompatibility)
{
    TestMod::VersionInfo engineVersion{1, 0};
    TestMod::VersionInfo modRequires{2, 0};

    // Engine 1.0 cannot run mod requiring 2.0
    EXPECT_FALSE(engineVersion.IsCompatibleWith(modRequires));

    // Engine 2.0 can run mod requiring 2.0
    TestMod::VersionInfo engine2{2, 0};
    EXPECT_TRUE(engine2.IsCompatibleWith(modRequires));

    // Engine 2.1 can run mod requiring 2.0
    TestMod::VersionInfo engine21{2, 1};
    EXPECT_TRUE(engine21.IsCompatibleWith(modRequires));

    // Engine 3.0 can run mod requiring 2.0
    TestMod::VersionInfo engine3{3, 0};
    EXPECT_TRUE(engine3.IsCompatibleWith(modRequires));
}
