// TestModuleHotReload.cpp - Tests for Spark::HotReload::ModuleHotReload
#include "TestFramework.h"
#include "Engine/HotReload/ModuleHotReload.h"

#include <filesystem>
#include <fstream>

// ============================================================================
// Initialization
// ============================================================================

TEST(ModuleHotReload_Initialize)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    EXPECT_TRUE(hr.IsEnabled());
    EXPECT_EQ(hr.GetWatchedModuleCount(), static_cast<size_t>(0));
    EXPECT_EQ(hr.GetTotalReloadCount(), static_cast<uint32_t>(0));
    hr.Shutdown();
}

TEST(ModuleHotReload_Shutdown_ClearsState)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    hr.RegisterModule("TestModule", "/tmp/test.dll");
    hr.Shutdown();
    EXPECT_EQ(hr.GetWatchedModuleCount(), static_cast<size_t>(0));
    EXPECT_FALSE(hr.IsEnabled());
}

// ============================================================================
// Module registration
// ============================================================================

TEST(ModuleHotReload_RegisterModule)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    EXPECT_TRUE(hr.RegisterModule("GameFPS", "/tmp/SparkGameFPS.dll"));
    EXPECT_EQ(hr.GetWatchedModuleCount(), static_cast<size_t>(1));
    auto names = hr.GetWatchedModuleNames();
    EXPECT_EQ(names.size(), static_cast<size_t>(1));
    EXPECT_EQ(names[0], std::string("GameFPS"));
    hr.Shutdown();
}

TEST(ModuleHotReload_RegisterModule_EmptyName_Fails)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    EXPECT_FALSE(hr.RegisterModule("", "/tmp/test.dll"));
    EXPECT_EQ(hr.GetWatchedModuleCount(), static_cast<size_t>(0));
    hr.Shutdown();
}

TEST(ModuleHotReload_UnregisterModule)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    hr.RegisterModule("TestMod", "/tmp/test.dll");
    hr.UnregisterModule("TestMod");
    EXPECT_EQ(hr.GetWatchedModuleCount(), static_cast<size_t>(0));
    hr.Shutdown();
}

TEST(ModuleHotReload_GetModule_Exists)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    hr.RegisterModule("MyMod", "/tmp/mymod.dll");
    auto* mod = hr.GetModule("MyMod");
    ASSERT_TRUE(mod != nullptr);
    EXPECT_EQ(mod->name, std::string("MyMod"));
    EXPECT_EQ(mod->reloadCount, static_cast<uint32_t>(0));
    hr.Shutdown();
}

TEST(ModuleHotReload_GetModule_NotExists)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    EXPECT_TRUE(hr.GetModule("NoSuchMod") == nullptr);
    hr.Shutdown();
}

// ============================================================================
// Configuration
// ============================================================================

TEST(ModuleHotReload_SetWatchDirectory)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    hr.SetWatchDirectory("/game/modules");
    auto status = hr.Console_GetStatus();
    EXPECT_TRUE(status.find("/game/modules") != std::string::npos);
    hr.Shutdown();
}

TEST(ModuleHotReload_SetEnabled)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    hr.SetEnabled(false);
    EXPECT_FALSE(hr.IsEnabled());
    hr.SetEnabled(true);
    EXPECT_TRUE(hr.IsEnabled());
    hr.Shutdown();
}

TEST(ModuleHotReload_ConsoleGetStatus)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    auto status = hr.Console_GetStatus();
    EXPECT_TRUE(status.find("ENABLED") != std::string::npos);
    EXPECT_TRUE(status.find("Watching: 0") != std::string::npos);
    hr.Shutdown();
}

TEST(ModuleHotReload_ForceReload_UnknownModule)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();
    auto result = hr.ForceReload("NonExistent");
    EXPECT_TRUE(result == Spark::HotReload::ReloadResult::FileNotFound);
    hr.Shutdown();
}

TEST(ModuleHotReload_FailedReplacementDoesNotUnloadWorkingModule)
{
    auto& hr = Spark::HotReload::ModuleHotReload::GetInstance();
    hr.Initialize();

    const std::filesystem::path modulePath = std::filesystem::temp_directory_path() / "SparkLegacyHotReloadFailure.dll";
    const std::filesystem::path sidecarPath = modulePath.string() + ".sparkabi";
    {
        std::ofstream module(modulePath, std::ios::binary | std::ios::trunc);
        module << "fixture";
        std::ofstream sidecar(sidecarPath, std::ios::trunc);
        sidecar << "fixture";
    }

    bool reloadCalled = false;
    bool unloadCalled = false;
    hr.RegisterModule("SparkLegacyHotReloadFailure", modulePath);
    hr.SetCallbacks([&](const std::string&) { unloadCalled = true; },
                    [&](const std::string&, const std::filesystem::path&)
                    {
                        reloadCalled = true;
                        return false;
                    });

    EXPECT_TRUE(hr.ForceReload("SparkLegacyHotReloadFailure") == Spark::HotReload::ReloadResult::LoadFailed);
    EXPECT_TRUE(reloadCalled);
    EXPECT_FALSE(unloadCalled);

    const auto* watched = hr.GetModule("SparkLegacyHotReloadFailure");
    if (watched)
    {
        std::error_code ec;
        std::filesystem::remove(watched->shadowPath.string() + ".sparkabi", ec);
        std::filesystem::remove(watched->shadowPath, ec);
    }
    hr.Shutdown();
    std::error_code ec;
    std::filesystem::remove(sidecarPath, ec);
    std::filesystem::remove(modulePath, ec);
}
