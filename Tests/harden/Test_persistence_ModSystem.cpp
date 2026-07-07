// Test_persistence_ModSystem.cpp
// Regression for the P2 ModSystem load-order finding: LoadEnabledMods sorted only by
// loadOrder (ignoring the dependency graph) and LoadMod checked only that dependencies
// were "enabled", never "loaded". A mod could therefore load before a dependency it
// needs. The fix topologically sorts enabled mods by their declared dependencies (with
// cycle detection) and LoadMod now requires each dependency to already be loaded.

#include "TestFramework.h"
#include "Engine/Modding/ModSystem.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Spark;

namespace
{
    std::string MakeTempModsDir(const char* name)
    {
        auto dir = std::filesystem::temp_directory_path() / (std::string("spark_harden_mods_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir.string();
    }

    void WriteMod(const std::string& modsDir, const std::string& id, const std::string& modJson)
    {
        auto modDir = std::filesystem::path(modsDir) / id;
        std::filesystem::create_directories(modDir);
        std::ofstream out((modDir / "mod.json").string(), std::ios::trunc);
        out << modJson;
    }
} // namespace

TEST(ModSystem_LoadEnabledMods_TopologicalOrderBeatsLoadOrder)
{
    const std::string dir = MakeTempModsDir("topo");

    // B depends on A. loadOrder alone would put B (0) before A (10) — a dependency
    // violation. The topological sort must load A before B regardless.
    WriteMod(dir, "A", R"({"id":"A","name":"ModA","version":"1.0","loadOrder":10})");
    WriteMod(dir, "B", R"({"id":"B","name":"ModB","version":"1.0","loadOrder":0,"dependencies":["A"]})");

    ModSystem mods;
    EXPECT_EQ(mods.ScanForMods(dir), static_cast<size_t>(2));
    EXPECT_TRUE(mods.EnableMod("A"));
    EXPECT_TRUE(mods.EnableMod("B"));

    std::vector<std::string> loadOrder;
    mods.OnModLoaded([&](const std::string& id) { loadOrder.push_back(id); });

    EXPECT_TRUE(mods.LoadEnabledMods());
    EXPECT_EQ(loadOrder.size(), static_cast<size_t>(2));
    if (loadOrder.size() == 2)
    {
        EXPECT_EQ(loadOrder[0], std::string("A"));
        EXPECT_EQ(loadOrder[1], std::string("B"));
    }
    EXPECT_TRUE(mods.IsModActive("A"));
    EXPECT_TRUE(mods.IsModActive("B"));

    std::filesystem::remove_all(dir);
}

TEST(ModSystem_LoadEnabledMods_DetectsDependencyCycle)
{
    const std::string dir = MakeTempModsDir("cycle");

    // C <-> D form a dependency cycle; LoadEnabledMods must fail rather than loop or
    // half-initialize.
    WriteMod(dir, "C", R"({"id":"C","name":"ModC","version":"1.0","dependencies":["D"]})");
    WriteMod(dir, "D", R"({"id":"D","name":"ModD","version":"1.0","dependencies":["C"]})");

    ModSystem mods;
    EXPECT_EQ(mods.ScanForMods(dir), static_cast<size_t>(2));
    EXPECT_TRUE(mods.EnableMod("C"));
    EXPECT_TRUE(mods.EnableMod("D"));

    EXPECT_FALSE(mods.LoadEnabledMods());

    std::filesystem::remove_all(dir);
}
