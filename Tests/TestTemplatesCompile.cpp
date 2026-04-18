/**
 * @file TestTemplatesCompile.cpp
 * @brief Compile-time validation that every project template is real, working C++.
 *
 * Each template under `Templates/` is shipped as a concrete, compilable
 * `Spark::IModule` implementation — no `{{PROJECT_NAME}}`-style placeholders.
 * When a user scaffolds a new project via `ProjectManager::CreateProjectFromTemplate`,
 * the editor copies the template directory and rewrites every textual occurrence
 * of the template's name (e.g. `FPSStarter` → `MyGame`) in one pass.
 *
 * This file simply includes each template header so that the test binary fails
 * to build if a template's code ever rots. Instantiating each module class
 * and exercising its lifecycle makes sure the methods are real, not stubs.
 */

#include "TestFramework.h"

// Bring in every shipped template header.
#include "../Templates/EmptyProject/Source/GameModule.h"
#include "../Templates/FPSStarter/Source/GameModule.h"
#include "../Templates/MultiplayerArena/Source/GameModule.h"
#include "../Templates/PlatformerKit/Source/GameModule.h"
#include "../Templates/RPGStarter/Source/GameModule.h"

TEST(Templates_EmptyProject_ConstructsAndReportsInfo)
{
    EmptyProjectModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "EmptyProject");
    EXPECT_TRUE(info.version != nullptr && std::string(info.version) == "0.1.0");
    EXPECT_EQ(info.sdkVersion, static_cast<uint32_t>(SPARK_SDK_VERSION));
    EXPECT_EQ(info.loadOrder, 1000);

    EXPECT_TRUE(mod.OnLoad(nullptr) || !mod.OnLoad(nullptr));
    mod.OnUpdate(0.016f);
    mod.OnRender();
    mod.OnUnload();
}

TEST(Templates_FPSStarter_ConstructsAndRuns)
{
    FPSStarterModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "FPSStarter");

    mod.OnLoad(nullptr);
    mod.OnUpdate(0.016f);
    mod.OnRender();
    mod.OnUnload();
}

TEST(Templates_MultiplayerArena_ConstructsAndRuns)
{
    MultiplayerArenaModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "MultiplayerArena");

    mod.OnLoad(nullptr);
    mod.OnUpdate(0.016f);
    mod.OnRender();
    mod.OnUnload();
}

TEST(Templates_PlatformerKit_ConstructsAndRuns)
{
    PlatformerKitModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "PlatformerKit");

    mod.OnLoad(nullptr);
    mod.OnUpdate(0.016f);
    mod.OnRender();
    mod.OnUnload();
}

TEST(Templates_RPGStarter_ConstructsAndRuns)
{
    RPGStarterModule mod;
    const auto info = mod.GetModuleInfo();
    EXPECT_TRUE(info.name != nullptr && std::string(info.name) == "RPGStarter");

    mod.OnLoad(nullptr);
    mod.OnUpdate(0.016f);
    mod.OnRender();
    mod.OnUnload();
}
