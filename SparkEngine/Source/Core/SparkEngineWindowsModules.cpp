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
#include "WindowsCommandLine.h"
#include "EngineContext.h"
#include "EngineRuntime.h"
#include "GameImGuiLayer.h"
#include "ModuleHotReload.h"
#include "ModuleManager.h"
#include "Utils/SparkConsole.h"
#ifdef SPARK_HAS_IMGUI
#include <imgui.h> // bare-launch project selector panel
#endif
#include <utility> // std::exchange (project selector choice hand-off)
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @brief Get the executable directory
 */
static std::filesystem::path GetExecutableDirectory()
{
    constexpr size_t kInitialPathCapacity = 512;
    constexpr size_t kMaximumPathCapacity = 32768;

    for (size_t capacity = kInitialPathCapacity; capacity <= kMaximumPathCapacity; capacity *= 2)
    {
        std::vector<wchar_t> buffer(capacity);
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
            return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
    }
    return {};
}

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
    const auto exeDir = GetExecutableDirectory();
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
            console.LogError("Explicit game module not found: " + *cmdLineModule);
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
 */
void DrawProjectSelectorPanel()
{
    if (g_projectSelectorCandidates.empty() || !g_projectSelectorChoice.empty())
        return;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("Spark Engine — Select Project", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::TextWrapped("Several game modules were found next to the engine. Pick one to launch.");
    ImGui::Separator();

    for (const auto& path : g_projectSelectorCandidates)
    {
        const std::string label = PathToUtf8(std::filesystem::u8path(path).stem());
        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 34.0f)))
            g_projectSelectorChoice = path;
    }

    ImGui::Separator();
    ImGui::Checkbox("Remember choice (writes spark.modules.json)", &g_projectSelectorRemember);
    ImGui::TextDisabled("Tip: launch with -game <dll> to skip this panel.");
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
        const std::filesystem::path executableDirectory = GetExecutableDirectory();
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
