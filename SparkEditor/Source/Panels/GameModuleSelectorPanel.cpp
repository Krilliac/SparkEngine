/**
 * @file GameModuleSelectorPanel.cpp
 * @brief Game module discovery, selection, and manifest generation
 */

#include "GameModuleSelectorPanel.h"
#include "Core/ModuleManager.h"
#include "../Utils/SparkConsole.h"
#include <imgui.h>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

// External: the global ModuleManager owned by SparkEngine
extern std::unique_ptr<ModuleManager> g_moduleManager;

namespace SparkEditor
{

    GameModuleSelectorPanel::GameModuleSelectorPanel() : EditorPanel("Game Module Selector", "GameModuleSelector") {}

    bool GameModuleSelectorPanel::Initialize()
    {
        m_isInitialized = true;
        RefreshModuleList();
        return true;
    }

    void GameModuleSelectorPanel::Update(float deltaTime)
    {
        m_refreshTimer += deltaTime;

        // Auto-refresh every 5 seconds to pick up newly built DLLs
        if (m_refreshTimer >= 5.0f)
        {
            m_refreshTimer = 0.0f;
            m_needsRefresh = true;
        }

        if (m_needsRefresh)
        {
            RefreshModuleList();
            m_needsRefresh = false;
        }
    }

    void GameModuleSelectorPanel::Render()
    {
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        ImGui::Text("Manage game module DLLs loaded by the engine.");
        ImGui::Separator();

        // Summary
        size_t loadedCount = 0;
        for (const auto& mod : m_modules)
        {
            if (mod.isLoaded)
                loadedCount++;
        }
        ImGui::Text("Available: %zu  |  Loaded: %zu", m_modules.size(), loadedCount);

        if (ImGui::Button("Refresh"))
            m_needsRefresh = true;
        ImGui::SameLine();
        if (ImGui::Button("Save Module Manifest"))
            SaveModuleManifest();

        if (!m_statusMessage.empty())
        {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "%s", m_statusMessage.c_str());
        }

        ImGui::Separator();
        RenderModuleList();

        ImGui::Separator();
        RenderManifestControls();

        EndPanel();
    }

    void GameModuleSelectorPanel::Shutdown()
    {
        m_modules.clear();
    }

    void GameModuleSelectorPanel::RefreshModuleList()
    {
        m_modules.clear();

        if (!g_moduleManager)
            return;

        // Get the executable directory for scanning
        std::string scanDir;
#ifdef _WIN32
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        scanDir = std::filesystem::path(exePath).parent_path().string();
#else
        scanDir = std::filesystem::canonical("/proc/self/exe").parent_path().string();
#endif

        auto discovered = g_moduleManager->DiscoverModules(scanDir);

        for (const auto& disc : discovered)
        {
            ModuleEntry entry;
            entry.name = disc.name;
            entry.path = disc.path;
            entry.version = disc.version;
            entry.isLoaded = disc.isLoaded;
            entry.isSelected = disc.isLoaded; // Pre-select loaded modules
            m_modules.push_back(std::move(entry));
        }
    }

    void GameModuleSelectorPanel::RenderModuleList()
    {
        if (m_modules.empty())
        {
            ImGui::TextDisabled("No game modules found in the output directory.");
            ImGui::TextDisabled("Build SparkGame or SparkGameMMO to see them here.");
            return;
        }

        ImGui::BeginChild("ModuleList", ImVec2(0, 250), true);

        for (size_t i = 0; i < m_modules.size(); ++i)
        {
            auto& mod = m_modules[i];
            ImGui::PushID(static_cast<int>(i));

            // Checkbox for manifest selection
            ImGui::Checkbox("##select", &mod.isSelected);
            ImGui::SameLine();

            // Status indicator
            if (mod.isLoaded)
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "[LOADED]");
            else
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[AVAILABLE]");
            ImGui::SameLine();

            // Module name and version
            ImGui::Text("%s", mod.name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("v%s", mod.version.c_str());

            // Show path on hover
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("Path: %s", mod.path.c_str());
                ImGui::EndTooltip();
            }

            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    void GameModuleSelectorPanel::RenderManifestControls()
    {
        ImGui::TextWrapped("The spark.modules.json manifest controls which modules load on startup. "
                           "Select modules above and click 'Save Module Manifest' to persist your choice. "
                           "The engine will load only the selected modules on next launch.");

        ImGui::Spacing();
        ImGui::TextDisabled("Tip: Use -game <path> on the command line to override the manifest.");
    }

    void GameModuleSelectorPanel::SaveModuleManifest()
    {
        std::string manifestDir;
#ifdef _WIN32
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        manifestDir = std::filesystem::path(exePath).parent_path().string();
#else
        manifestDir = std::filesystem::canonical("/proc/self/exe").parent_path().string();
#endif

        auto manifestPath = std::filesystem::path(manifestDir) / "spark.modules.json";

        // Build JSON manually (no dependency on a JSON library)
        std::string json = "{\n    \"modules\": [\n";
        bool first = true;
        int loadOrder = 1000;

        for (const auto& mod : m_modules)
        {
            if (!mod.isSelected)
                continue;

            if (!first)
                json += ",\n";
            first = false;

            auto filename = std::filesystem::path(mod.path).filename().string();
            json += "        {\n";
            json += "            \"name\": \"" + mod.name + "\",\n";
            json += "            \"path\": \"" + filename + "\",\n";
            json += "            \"loadOrder\": " + std::to_string(loadOrder) + "\n";
            json += "        }";
            loadOrder++;
        }

        json += "\n    ]\n}\n";

        std::ofstream file(manifestPath);
        if (file.is_open())
        {
            file << json;
            file.close();
            m_statusMessage = "Saved spark.modules.json (" + std::to_string(loadOrder - 1000) + " modules)";

            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogSuccess("[Editor] " + m_statusMessage);
        }
        else
        {
            m_statusMessage = "Failed to write spark.modules.json";
            auto& console = Spark::SimpleConsole::GetInstance();
            console.LogError("[Editor] " + m_statusMessage);
        }
    }

} // namespace SparkEditor
