/** @file main.cpp
 * @brief Production console dispatch and module cleanup for commands moved from FPS into the host.
 */
#include "Console/AdvancedConsoleCommands.h"
#include "Core/EngineContext.h"
#include "Core/ModuleManager.h"
#include "Core/SubsystemConsoleCommands.h"
#include "Graphics/GraphicsConsoleCommands.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/LightingSystem.h"
#include "Graphics/MaterialSystem.h"
#include "Graphics/PostProcessingPipeline.h"
#include "Utils/SparkConsole.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH
#error SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH must name the production registry fixture image
#endif

namespace
{
    constexpr const char* kFpsOwner = "FPSHostConsoleBoundary.FPS";
    constexpr const char* kHostCommands[] = {"mat_list", "mat_info",    "light_list", "shadows",
                                             "pp_list",  "pp_exposure", "exposure"};

    bool SetFixtureSwitch(const char* name, const char* value)
    {
#ifdef _WIN32
        return _putenv_s(name, value) == 0;
#else
        return (*value ? setenv(name, value, 1) : unsetenv(name)) == 0;
#endif
    }

    std::string Dispatch(Spark::SimpleConsole& console, const std::string& command)
    {
        if (!console.ExecuteCommand(command))
            return "<dispatch failed: " + command + ">";
        const auto history = console.GetLogHistory();
        return history.empty() ? "<no command output>" : history.back().message;
    }

    struct ConsoleShutdown
    {
        Spark::SimpleConsole& console;
        Spark::IConsole& sdk;
        ~ConsoleShutdown()
        {
            {
                const Spark::SimpleConsole::ScopedRegistrationOwner owner(console, kFpsOwner);
                SparkConsole::UnregisterAdvancedCommands(sdk);
            }
            console.UnregisterCommandsByOwner(kFpsOwner);
            console.Shutdown();
            EngineContext::SetInjected(nullptr);
        }
    };
} // namespace

int main()
{
    // This executable owns its process environment; do not let external fixture
    // switches write sentinel files or change the intended lifecycle scenarios.
    for (const char* name : {"SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", "SPARK_REGISTRY_FIXTURE_THROW_ON_UNLOAD",
                             "SPARK_REGISTRY_FIXTURE_LIFECYCLE_SENTINEL", "SPARK_REGISTRY_FIXTURE_CODE_ADDRESS_FILE"})
    {
        if (!SetFixtureSwitch(name, ""))
            return 1;
    }

    int failures = 0;
    auto check = [&failures](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };

    GraphicsEngine graphics;
#ifndef _WIN32
    // Linux creates these CPU-side systems during its real NullRHI startup;
    // Windows creates them in the constructor, with no device/window needed.
    if (FAILED(graphics.Initialize(nullptr)))
    {
        std::cerr << "NullRHI graphics initialization failed\n";
        return 1;
    }
#endif
    EngineContext context;
    context.SetGraphics(&graphics);
    EngineContext::SetInjected(&context);
    auto& console = Spark::SimpleConsole::GetInstance();
    Spark::IConsole& sdk = *context.GetConsole();
    const ConsoleShutdown cleanup{console, sdk};
    if (!console.Initialize())
        return 1;
    console.SetCurrentPermissionLevel(Spark::CommandPermission::Developer);
    Spark::Graphics::RegisterGraphicsConsoleCommands(graphics);
    Spark::RegisterSubsystemConsoleCommands();

    auto* materials = graphics.GetMaterialSystem();
    auto* lighting = graphics.GetLightingSystem();
    auto* postProcessing = graphics.GetPostProcessingPipeline();
    if (!materials || !lighting || !postProcessing)
    {
        std::cerr << "Production graphics subsystems are unavailable\n";
        return 1;
    }
    check(materials->CreateMaterial("Console Boundary Material") != nullptr, "Create the real material fixture");
    check(Dispatch(console, "mat_list").find("Console Boundary Material") != std::string::npos,
          "Host material list must use the real material registry before any module loads");
    check(Dispatch(console, "mat_info \"Console Boundary Material\"").find("Console Boundary Material") !=
              std::string::npos,
          "Quoted material name must arrive as the first argument through production dispatch");
    check(Dispatch(console, "mat_info") == "Usage: mat_info <material_name>",
          "Missing material argument must show usage");
    check(Dispatch(console, "mat_info first second") == "Usage: mat_info <material_name>",
          "Extra material arguments must not silently select the wrong material");
    check(Dispatch(console, "light_list").starts_with("Lighting System - Active Lights ("),
          "Host light list must dispatch before module initialization");
    check(Dispatch(console, "shadows off") == "Shadows disabled" && !lighting->AreShadowsEnabled(),
          "Host shadows off must mutate the real lighting state using args[0]");
    check(Dispatch(console, "shadows on") == "Shadows enabled" && lighting->AreShadowsEnabled(),
          "Host shadows on must enable the real lighting state");
    check(Dispatch(console, "shadows typo") == "Usage: shadows <on/off>" && lighting->AreShadowsEnabled(),
          "Invalid shadow input must leave lighting state unchanged");

    for (const char* name : {"pp_exposure", "exposure"})
    {
        const std::string command(name);
        check(Dispatch(console, command + " 1.75") == "Exposure set to 1.75" &&
                  std::abs(postProcessing->GetLightShaftSettings().exposure - 1.75f) < 0.0001f,
              "Both host exposure names must mutate the same production state");
        for (const char* invalid : {"nan", "inf", "-inf", "1.25junk", "1e100", "0x1p2", "\" 2\"", "\"2 \""})
        {
            check(Dispatch(console, command + " " + invalid).starts_with("Invalid number") &&
                      postProcessing->GetLightShaftSettings().exposure == 1.75f,
                  "Invalid exposure must reject the entire token and preserve state");
        }
        check(Dispatch(console, command).starts_with("Usage:") &&
                  Dispatch(console, command + " 2 3").starts_with("Usage:") &&
                  postProcessing->GetLightShaftSettings().exposure == 1.75f,
              "Missing and extra exposure arguments must preserve state");
    }
    const std::string hostPostProcessList = Dispatch(console, "pp_list");
    check(hostPostProcessList.starts_with("Post-Processing Pipeline:"),
          "The original host pp_list must return production effect information");

    // Invoke the actual FPS registrar through the actual public SDK adapter.
    // Moved host commands must never become part of the module's cleanup set.
    {
        const Spark::SimpleConsole::ScopedRegistrationOwner owner(console, kFpsOwner);
        SparkConsole::RegisterAdvancedCommands(sdk, nullptr, &graphics);
        for (const char* name : kHostCommands)
        {
            check(!sdk.RegisterCommand(
                      name, [](const std::vector<std::string>&) { return "module hijack"; }, "duplicate test", "Tests",
                      ""),
                  "The module must not replace a host command through IConsole");
            sdk.UnregisterCommand(name);
        }
    }
    for (const char* name : kHostCommands)
        check(console.HasCommand(name) && console.GetCommandOwner(name).empty(),
              "Migrated commands must remain host-owned");
    check(Dispatch(console, "vsync off") == "VSync disabled" && !graphics.GetGraphicsSettings().vsync,
          "FPS vsync must consume the first SDK argument");
    check(Dispatch(console, "vsync on") == "VSync enabled" && graphics.GetGraphicsSettings().vsync,
          "FPS vsync must support the opposite state transition");
    check(Dispatch(console, "tex_memory nan") == "Invalid number: nan", "FPS numeric validation must receive args[0]");
    check(Dispatch(console, "gravity 1 2 3") == "Physics system not available",
          "FPS gravity must accept exactly three supplied components before checking the service");
    check(Dispatch(console, "gravity 1 2") == "Usage: gravity <x> <y> <z>",
          "Short gravity input must not index past args");
    {
        const Spark::SimpleConsole::ScopedRegistrationOwner owner(console, kFpsOwner);
        SparkConsole::UnregisterAdvancedCommands(sdk);
    }
    check(!console.HasCommand("tex_memory") && !console.HasCommand("vsync"),
          "FPS unregister must remove its own callbacks");
    check(Dispatch(console, "pp_list") == hostPostProcessList, "FPS unregister must retain the original host pp_list");

    for (bool failOnLoad : {false, true})
    {
        if (!SetFixtureSwitch("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", failOnLoad ? "1" : ""))
            return 1;
        ModuleManager manager;
        const bool loaded = manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH);
        check(loaded, "Load the real module registry lifecycle fixture");
        if (loaded)
        {
            const bool initialized = manager.InitializeAll(&context);
            check(initialized == !failOnLoad, "Module initialization must match the normal/failed-load scenario");
            if (!failOnLoad)
                check(console.HasCommand("registry_fixture_status"),
                      "Normal module load must register its own command");
            manager.UnloadAll();
            check(!console.HasCommand("registry_fixture_status"),
                  "Unload or failed-load cleanup must remove module commands");
        }
        for (const char* name : kHostCommands)
            check(console.HasCommand(name) && console.GetCommandOwner(name).empty(),
                  "Host commands must survive actual ModuleManager unload and failed-load cleanup");
        check(Dispatch(console, "shadows off") == "Shadows disabled" && !lighting->AreShadowsEnabled(),
              "Host lighting command must still operate after module cleanup");
        check(Dispatch(console, "exposure 2.5") == "Exposure set to 2.5" &&
                  postProcessing->GetLightShaftSettings().exposure == 2.5f,
              "Host exposure command must still operate after module cleanup");
    }
    return failures == 0 ? 0 : 1;
}
