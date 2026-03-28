// TestModSystem.cpp - Tests for game modding system

#include "TestFramework.h"
#include "Engine/Modding/ModSystem.h"

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
