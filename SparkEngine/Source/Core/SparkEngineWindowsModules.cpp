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
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS

/**
 * @brief Get the executable directory
 */
static std::filesystem::path GetExecutableDirectory()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path();
}

/**
 * @brief Find a specific game module DLL from command line
 *
 * Checks for -game <path> on the command line.
 * Returns empty string if not specified.
 */
static std::string FindGameModuleFromCmdLine(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    size_t pos = cmd.find(L"-game ");
    if (pos != std::wstring::npos)
    {
        size_t start = pos + 6;
        size_t end = cmd.find(L' ', start);
        std::wstring wpath = cmd.substr(start, end - start);
        std::string path(wpath.begin(), wpath.end());
        // error_code overload: don't let a malformed -game value throw from main.
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec)
            return path;
    }
    return "";
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
 * 2. spark.modules.json manifest next to the engine exe
 * 3. Bare launch: discover candidates WITHOUT loading. One candidate loads
 *    directly; several arm the project selector panel (windowed) or print
 *    pick-one guidance (headless). The old load-everything scan is gone —
 *    ModuleManager also hard-refuses a second Game-kind module now.
 */
bool LoadGameModules(ModuleManager& manager, LPWSTR cmdLine)
{
    auto exeDir = GetExecutableDirectory();
    auto& console = Spark::SimpleConsole::GetInstance();

    // 1. Check command line for specific module
    std::string cmdLineModule = FindGameModuleFromCmdLine(cmdLine);
    if (!cmdLineModule.empty())
        return manager.LoadModule(cmdLineModule);

    // 2. Check for module manifest
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(manifestPath.string());

    // 3. Bare launch: discover, never bulk-load.
    auto candidates = ModuleManager::DiscoverModuleCandidates(exeDir.string());
    if (candidates.empty())
        return false; // caller prints the existing "no game modules" guidance

    if (candidates.size() == 1)
    {
        console.LogInfo("Single module candidate — loading " +
                        std::filesystem::path(candidates.front()).filename().string());
        return manager.LoadModule(candidates.front());
    }

    g_projectSelectorCandidates = std::move(candidates);
    console.LogInfo(std::format("{} module candidates found — project selector armed (pass -game <dll> to skip)",
                                g_projectSelectorCandidates.size()));
    for (const auto& c : g_projectSelectorCandidates)
        console.LogInfo("  candidate: " + std::filesystem::path(c).filename().string());
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
        const std::string label = std::filesystem::path(path).stem().string();
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
    {
        auto info = primary->GetModuleInfo();
        std::wstring title = L"Spark Engine - ";
        std::string modName(info.name);
        title.append(modName.begin(), modName.end());
        if (HWND hWnd = FindWindowW(g_szClass, nullptr))
            SetWindowTextW(hWnd, title.c_str());
    }

    if (rt.moduleHotReload)
        rt.moduleHotReload->WatchAllLoadedModules();

    if (g_projectSelectorRemember)
    {
        // Manifest parser keys off "path" entries — keep the format minimal.
        std::ofstream manifest(GetExecutableDirectory() / "spark.modules.json");
        if (manifest)
        {
            const std::string fname = std::filesystem::path(path).filename().string();
            manifest << "{\n  \"modules\": [\n    { \"name\": \"" << std::filesystem::path(path).stem().string()
                     << "\", \"path\": \"" << fname << "\" }\n  ]\n}\n";
            console.LogInfo("Project selector: wrote spark.modules.json (delete it to get the selector back)");
        }
    }

    g_projectSelectorCandidates.clear();
#ifdef SPARK_HAS_IMGUI
    Spark::GameImGui::SetAuxPanel(nullptr);
#endif
    console.LogSuccess("Project selector: launched " + std::filesystem::path(path).filename().string());
}

#endif // SPARK_PLATFORM_WINDOWS
