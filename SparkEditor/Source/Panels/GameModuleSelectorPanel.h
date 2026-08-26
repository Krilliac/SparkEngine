/**
 * @file GameModuleSelectorPanel.h
 * @brief Panel for discovering game modules and launching the game as a separate process
 * @author Spark Engine Team
 * @date 2026
 *
 * The editor's cockpit for game modules:
 * - Lists module DLL candidates found next to the editor (safe scan — no DllMain)
 * - On-demand metadata probe (name / version / ModuleKind) via ModuleManager::DiscoverModules
 * - Shows the single-game-module policy (exactly ONE Game-kind module loads per process)
 * - Launches the selected module in a SEPARATE SparkEngine.exe process
 *   ("-game <dll>"), windowed or dedicated/headless ("-headless"), and tracks
 *   the last-launch status. In-editor play is deliberately NOT offered: module
 *   DLLs statically link the engine, so per-image globals and type-ids make
 *   in-process play unsafe.
 * - Generates spark.modules.json for persistent module selection
 *
 * Accessible via Window > Game Module Selector.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Utils/EditorProcessLaunch.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Editor panel for discovering, selecting, and launching game module DLLs
     */
    class GameModuleSelectorPanel : public EditorPanel
    {
      public:
        GameModuleSelectorPanel();
        ~GameModuleSelectorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "GameModuleSelectorPanel"; }

        /// @brief Path of the module currently chosen for launch (radio selection),
        /// or empty if none selected. Consumed by PlayControlPanel (W13) so its play
        /// bar launches the same module the user picked here, instead of duplicating
        /// module discovery/probing.
        const std::string& GetLaunchSelectionPath() const { return m_launchSelectionPath; }

      private:
        void RefreshModuleList();
        void RenderModuleList();
        void RenderLaunchControls();
        void RenderManifestControls();
        void SaveModuleManifest();
        void LaunchGame(bool headless);
        void PollLaunchedProcess();
        void StopLaunchedProcess();

        struct ModuleEntry
        {
            std::string name;        ///< Display name (filename stem until probed)
            std::string path;        ///< Full path to the DLL
            std::string version;     ///< Version string ("?" until probed)
            std::string kindLabel;   ///< "Game" / "Addon" / "?" (unprobed)
            bool isGameKind = true;  ///< Counts against the single-game-module policy
            bool kindKnown = false;  ///< True once metadata was probed
            bool isSelected = false; ///< User selection for manifest generation
        };

        std::vector<ModuleEntry> m_modules;
        float m_refreshTimer = 0.0f;
        bool m_needsRefresh = true;
        std::string m_statusMessage;

        // --- Launch state (separate-process game launch) ---
        int m_launchSelection = -1;        ///< Index into m_modules chosen for launch
        std::string m_launchSelectionPath; ///< Path of the selection (survives refresh reordering)
        OwnedEditorProcess m_gameProcess;  ///< Owns the launched SparkEngine process tree.
        std::string m_launchStatus;        ///< Human-readable last-launch status line
    };

} // namespace SparkEditor
