/**
 * @file SparkEngineWindowsModules.cpp
 * @brief Game-module discovery, -game/manifest loading, and the bare-launch project selector (Windows).
 *
 * Split from SparkEngineWindows.cpp to keep files under the ~500-line guideline.
 * Contains LoadGameModules (command line -> manifest -> discovery priority),
 * the ImGui project-selector panel, and the deferred selector-choice consumer
 * used by the main loop. The wWinMain entry point stays in SparkEngineWindows.cpp.
 */
#include "SparkEngine.h"
#include "Platform.h"
#include "framework.h"
#include "SparkEngineWindowsInternal.h"
#include "RuntimePackage.h"
#include "WindowsCommandLine.h"
#include "EngineContext.h"
#include "EngineRuntime.h"
#include "GameImGuiLayer.h"
#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#ifdef SPARK_HAS_IMGUI
#include <SparkUI/Brand.h>   // bare-launch project selector: brand header
#include <SparkUI/Fonts.h>   // ... font roles
#include <SparkUI/Glyphs.h>  // ... empty-state glyph
#include <SparkUI/Runtime.h> // ... Em()
#include <SparkUI/Widgets.h> // ... buttons, text roles, empty state
#include <imgui.h>           // bare-launch project selector panel
#endif
#include <algorithm> // std::max (project selector row layout)
#include <cstddef>
#include <utility> // std::exchange (project selector choice hand-off)
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS

static std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

// ============================================================================
// Bare-launch project selector state
//
// When the engine starts with no -game flag and no manifest it no longer
// bulk-loads every module DLL it can find (that co-loaded up to 10 game
// modules, double-stepped physics, and corrupted shared services). Instead
// the candidates are listed here and a small ImGui panel lets the user pick
// ONE. The pick is deferred to the main loop (g_projectSelectorChoice) —
// loading a DLL from inside the ImGui render callback would run module init
// mid-frame.
// ============================================================================
std::vector<std::string> g_projectSelectorCandidates;
static std::string g_projectSelectorChoice; ///< set by the panel, consumed by the main loop
static bool g_projectSelectorRemember = false;

/**
 * @brief Find the module manifest or fall back to the project selector
 *
 * Loading priority:
 * 1. Command line: -game <path> (loads single module)
 * 2. Command line: -manifest <path> (loads the explicit project manifest)
 * 3. spark.modules.json manifest next to the engine exe
 * 4. Bare launch: discover candidates WITHOUT loading. One candidate loads
 *    directly; several arm the project selector panel (windowed) or print
 *    pick-one guidance (headless). The old load-everything scan is gone —
 *    ModuleManager also hard-refuses a second Game-kind module now.
 */
bool LoadGameModules(ModuleManager& manager, LPWSTR cmdLine)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const auto exeDir = Spark::RuntimePackage::GetExecutableDirectory();
    if (exeDir.empty())
    {
        console.LogError("Could not determine the SparkEngine executable directory");
        return false;
    }

    // 1. Check command line for specific module
    const bool hasGameOption = Spark::Platform::HasWindowsCommandLineOption(cmdLine, L"-game");
    const auto cmdLineModule = Spark::Platform::FindWindowsCommandLineUtf8Argument(cmdLine, L"-game");
    if (hasGameOption)
    {
        if (!cmdLineModule || cmdLineModule->empty())
        {
            console.LogError("-game requires a non-empty path");
            return false;
        }

        const std::filesystem::path requestedModule = std::filesystem::u8path(*cmdLineModule);
        std::error_code moduleError;
        if (!std::filesystem::is_regular_file(requestedModule, moduleError) || moduleError)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "Explicit game module not found: %s", cmdLineModule->c_str());
            return false;
        }
        return manager.LoadModule(*cmdLineModule);
    }

    // 2. An explicit project manifest wins over the engine-directory fallback.
    const bool hasManifestOption = Spark::Platform::HasWindowsCommandLineOption(cmdLine, L"-manifest");
    const auto explicitManifest = Spark::Platform::FindWindowsCommandLineUtf8Argument(cmdLine, L"-manifest");
    if (hasManifestOption)
    {
        if (!explicitManifest || explicitManifest->empty())
        {
            console.LogError("-manifest requires a non-empty path");
            return false;
        }

        const std::filesystem::path requestedManifest = std::filesystem::u8path(*explicitManifest);
        std::error_code manifestError;
        if (!std::filesystem::is_regular_file(requestedManifest, manifestError) || manifestError)
        {
            console.LogError("Explicit module manifest not found: " + *explicitManifest);
            return false;
        }
        return manager.LoadModulesFromManifest(*explicitManifest);
    }

    // 3. Check for the engine-directory module manifest.
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(PathToUtf8(manifestPath));

    // 4. Bare launch: discover, never bulk-load.
    auto candidates = ModuleManager::DiscoverModuleCandidates(PathToUtf8(exeDir));
    if (candidates.empty())
        return false; // caller prints the existing "no game modules" guidance

    if (candidates.size() == 1)
    {
        console.LogInfo("Single module candidate — loading " +
                        PathToUtf8(std::filesystem::u8path(candidates.front()).filename()));
        return manager.LoadModule(candidates.front());
    }

    g_projectSelectorCandidates = std::move(candidates);
    console.LogInfo(std::format("{} module candidates found — project selector armed (pass -game <dll> to skip)",
                                g_projectSelectorCandidates.size()));
    for (const auto& c : g_projectSelectorCandidates)
        console.LogInfo("  candidate: " + PathToUtf8(std::filesystem::u8path(c).filename()));
    return false; // nothing loaded yet; the selector panel drives the pick
}

#ifdef SPARK_HAS_IMGUI
/**
 * @brief ImGui panel listing discovered game modules (bare-launch flow).
 *
 * Runs inside GameImGui::RenderOverlay on the main thread. Only records the
 * choice — the actual module load happens in the main loop next frame.
 * Drawn with the Spark UI kit: brand header, one row per module (name over
 * file name, Launch on the right) and an empty state when nothing is armed.
 */
void DrawProjectSelectorPanel()
{
    if (!g_projectSelectorChoice.empty())
        return;

    namespace Ui = Spark::Ui;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Ui::Em(32.0f), 0.0f), ImGuiCond_Always);
    ImGui::Begin("Select Project###SparkProjectSelector", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);

    Ui::BrandHeader("Spark Engine", "Select a project to launch");
    ImGui::Spacing();

    if (g_projectSelectorCandidates.empty())
    {
        Ui::EmptyState(Ui::Glyph::kCircleInfo, "No game modules found",
                       "Place a game module next to the engine executable, or launch with -game <dll>.");
        ImGui::End();
        return;
    }

    Ui::SubHeader("Game modules next to the engine");
    const float launchWidth = Ui::Em(6.0f);
    for (std::size_t index = 0; index < g_projectSelectorCandidates.size(); ++index)
    {
        const std::filesystem::path modulePath = std::filesystem::u8path(g_projectSelectorCandidates[index]);
        const std::string name = PathToUtf8(modulePath.stem());
        const std::string file = PathToUtf8(modulePath.filename());

        ImGui::PushID(static_cast<int>(index));
        const float rowStartX = ImGui::GetCursorPosX();
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const float rowTopY = ImGui::GetCursorPosY();
        ImGui::BeginGroup();
        {
            Ui::ScopedFont strong(Ui::FontRole::Strong);
            ImGui::TextUnformatted(name.c_str());
        }
        {
            Ui::ScopedFont monoSmall(Ui::FontRole::MonoSmall);
            Ui::SecondaryText("Game module  %s", file.c_str());
        }
        ImGui::EndGroup();
        const float rowHeight = ImGui::GetItemRectSize().y;

        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(rowStartX + rowWidth - launchWidth,
                                   rowTopY + std::max(0.0f, (rowHeight - ImGui::GetFrameHeight()) * 0.5f)));
        if (Ui::PrimaryButton("Launch", ImVec2(launchWidth, 0.0f)))
            g_projectSelectorChoice = g_projectSelectorCandidates[index];
        ImGui::PopID();

        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Checkbox("Remember choice (writes spark.modules.json)", &g_projectSelectorRemember);
    Ui::SecondaryText("Tip: launch with -game <dll> to skip this panel.");
    ImGui::End();
}
#endif // SPARK_HAS_IMGUI

/**
 * @brief Consume a project-selector pick: load + init the chosen module.
 *
 * Called from the main loop (never from the ImGui frame). Mirrors the
 * relevant parts of LoadAndInitModules for a late, post-startup load.
 */
void ConsumeProjectSelectorChoice()
{
    if (g_projectSelectorChoice.empty())
        return;

    const std::string path = std::exchange(g_projectSelectorChoice, {});
    auto& console = Spark::SimpleConsole::GetInstance();
    auto& rt = GetEngineRuntime();
    if (!rt.moduleManager)
        return;

    if (!rt.moduleManager->LoadModule(path))
    {
        // Choice already consumed; candidates stay armed so the panel simply
        // reappears next frame for another pick.
        console.LogError("Project selector: failed to load " + path);
        return;
    }

    rt.moduleManager->InitializeAll(EngineContext::Get());

    if (auto* primary = rt.moduleManager->GetPrimaryModule())
        ApplyRuntimeWindowCaption();

    if (rt.moduleHotReload)
        rt.moduleHotReload->WatchAllLoadedModules();

    if (g_projectSelectorRemember)
    {
        // Manifest parser keys off "path" entries — keep the format minimal.
        const std::filesystem::path executableDirectory = Spark::RuntimePackage::GetExecutableDirectory();
        if (executableDirectory.empty())
        {
            console.LogError("Project selector: could not determine the executable directory; choice was not saved");
        }
        else
        {
            std::ofstream manifest(executableDirectory / "spark.modules.json");
            if (manifest)
            {
                const std::filesystem::path modulePath = std::filesystem::u8path(path);
                const std::string fname = PathToUtf8(modulePath.filename());
                manifest << "{\n  \"modules\": [\n    { \"name\": \"" << PathToUtf8(modulePath.stem())
                         << "\", \"path\": \"" << fname << "\" }\n  ]\n}\n";
                console.LogInfo("Project selector: wrote spark.modules.json (delete it to get the selector back)");
            }
        }
    }

    g_projectSelectorCandidates.clear();
#ifdef SPARK_HAS_IMGUI
    Spark::GameImGui::SetAuxPanel(nullptr);
#endif
    console.LogSuccess("Project selector: launched " + PathToUtf8(std::filesystem::u8path(path).filename()));
}

#endif // SPARK_PLATFORM_WINDOWS
