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

// SEC-120 mod-manifest: a rescan (the ModdingPanel rescan button) used to overwrite
// every entry with a freshly parsed ModInfo, so an Active mod came back loaded=false and
// Available without its unload callbacks running, and UnloadAll then skipped it: the
// subscriber-owned assets of that mod were never released.
TEST(ModSystem_RescanKeepsActiveModLoadedAndUnloadable)
{
    const std::string dir = MakeTempModsDir("rescan");
    WriteMod(dir, "Keep", R"({"id":"Keep","name":"Keep","version":"1.0"})");

    ModSystem mods;
    EXPECT_EQ(mods.ScanForMods(dir), static_cast<size_t>(1));
    EXPECT_TRUE(mods.EnableMod("Keep"));
    EXPECT_TRUE(mods.LoadMod("Keep"));
    int unloads = 0;
    mods.OnModUnloaded([&](const std::string&) { ++unloads; });

    // The rescan refreshes the manifest metadata but never the load state.
    WriteMod(dir, "Keep", R"({"id":"Keep","name":"Keep Renamed","version":"1.1"})");
    EXPECT_EQ(mods.ScanForMods(dir), static_cast<size_t>(1));
    EXPECT_TRUE(mods.IsModActive("Keep"));
    const ModInfo* info = mods.GetModInfo("Keep");
    ASSERT_TRUE(info != nullptr);
    EXPECT_TRUE(info->enabled);
    EXPECT_TRUE(info->loaded);
    EXPECT_EQ(info->name, std::string("Keep Renamed"));
    EXPECT_EQ(info->version, std::string("1.1"));

    mods.UnloadAll();
    EXPECT_EQ(unloads, 1);
    EXPECT_FALSE(mods.IsModActive("Keep"));

    std::filesystem::remove_all(dir);
}

TEST(ModSystem_RescanKeepsActiveModPathOwnership)
{
    const std::string originalDir = MakeTempModsDir("rescan_path_original");
    const std::string replacementDir = MakeTempModsDir("rescan_path_replacement");
    WriteMod(originalDir, "Keep", R"({"id":"Keep","name":"Keep","version":"1.0"})");
    WriteMod(replacementDir, "Keep", R"({"id":"Keep","name":"Replacement","version":"2.0"})");

    ModSystem mods;
    EXPECT_EQ(mods.ScanForMods(originalDir), static_cast<size_t>(1));
    EXPECT_TRUE(mods.EnableMod("Keep"));
    EXPECT_TRUE(mods.LoadMod("Keep"));
    const ModInfo* info = mods.GetModInfo("Keep");
    ASSERT_TRUE(info != nullptr);
    const std::string originalPath = info->path;

    // A different scan root can discover the same id at a replacement path. The active
    // resource owner remains anchored to the path used for the successful load.
    EXPECT_EQ(mods.ScanForMods(replacementDir), static_cast<size_t>(1));
    info = mods.GetModInfo("Keep");
    ASSERT_TRUE(info != nullptr);
    EXPECT_EQ(info->path, originalPath);
    EXPECT_EQ(info->name, std::string("Replacement"));
    EXPECT_EQ(info->version, std::string("2.0"));
    EXPECT_TRUE(mods.IsModActive("Keep"));

    mods.UnloadAll();
    EXPECT_FALSE(mods.IsModActive("Keep"));

    std::filesystem::remove_all(originalDir);
    std::filesystem::remove_all(replacementDir);
}

// Two directories declaring one id used to publish whichever the directory iterator
// reached last (a dropped-in mod could shadow an installed one), and ScanForMods counted
// both although only one was registered.
TEST(ModSystem_DuplicateIdAcrossDirectoriesIsNotPublished)
{
    const std::string dir = MakeTempModsDir("duplicate");
    WriteMod(dir, "a", R"({"id":"same","name":"First","version":"1.0"})");
    WriteMod(dir, "b", R"({"id":"same","name":"Second","version":"1.0"})");
    WriteMod(dir, "c", R"({"id":"other","name":"Other","version":"1.0"})");

    ModSystem mods;
    EXPECT_EQ(mods.ScanForMods(dir), static_cast<size_t>(1));
    EXPECT_TRUE(mods.GetModInfo("same") == nullptr);
    EXPECT_TRUE(mods.GetModInfo("other") != nullptr);
    EXPECT_EQ(mods.GetAllMods().size(), static_cast<size_t>(1));

    std::filesystem::remove_all(dir);
}

// The id is the map key, a log argument, a SaveConfig field and a UI label; it used to
// accept anything non-empty, including separators and control bytes (\u0000 included).
TEST(ModSystem_RejectsIdWithControlOrSeparatorBytes)
{
    const std::string dir = MakeTempModsDir("idpolicy");
    WriteMod(dir, "slash", R"({"id":"a/b","name":"Slash","version":"1.0"})");
    WriteMod(dir, "control", R"({"id":"x\u0001y","name":"Control","version":"1.0"})");
    WriteMod(dir, "nul", R"({"id":"x\u0000y","name":"Nul","version":"1.0"})");
    WriteMod(dir, "dotdot", R"({"id":"..","name":"DotDot","version":"1.0"})");
    WriteMod(dir, "space", R"({"id":" ","name":"Space","version":"1.0"})");
    WriteMod(dir, "long", std::string(R"({"id":")") + std::string(129, 'a') + R"(","name":"Long"})");
    WriteMod(dir, "selfdep", R"({"id":"selfdep","name":"SelfDep","dependencies":["selfdep"]})");
    WriteMod(dir, "good", R"({"id":"Good.Mod-1_x","name":"Good","dependencies":["dep","dep"]})");

    ModSystem mods;
    EXPECT_EQ(mods.ScanForMods(dir), static_cast<size_t>(1));
    const std::vector<ModInfo> all = mods.GetAllMods();
    ASSERT_EQ(all.size(), static_cast<size_t>(1));
    EXPECT_EQ(all[0].id, std::string("Good.Mod-1_x"));
    // A dependency listed twice is recorded once.
    EXPECT_EQ(all[0].dependencies.size(), static_cast<size_t>(1));

    std::filesystem::remove_all(dir);
}
