/**
 * @file PlayControlPanel.h
 * @brief Out-of-process play-testing controls: launch/stop real SparkEngine.exe
 *        instances (game / dedicated+bots / client), watch exec_audit.log, quick-connect.
 * @author Spark Engine Team
 * @date 2026
 *
 * Builds on W9's GameModuleSelectorPanel (module discovery + single "Launch Game" /
 * "Launch Dedicated" buttons). This panel is the play-control BAR: it reuses the
 * module currently selected there (GameModuleSelectorPanel::GetLaunchSelectionPath)
 * and the shared CreateProcessW helper (Utils/EditorProcessLaunch.h) to spawn and
 * track MULTIPLE concurrently-running instances with distinct roles:
 *   - Launch Game       : windowed client, no -exec.
 *   - Launch Dedicated + N bots : headless server, -exec runs `tf_dedicated` then
 *     `tf_chaos N` from an editor-generated temp cfg.
 *   - Launch Client (Connecting-To) : windowed client, -exec runs `tf_connect <host:port>`.
 *   - Stop All           : force-terminates every tracked instance.
 *
 * In-editor play is deliberately NOT offered here either — see
 * GameModuleSelectorPanel's header comment (module DLLs statically link the engine;
 * per-image globals/type-ids make in-process play unsafe).
 *
 * Accessible via Window > Play Control.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <filesystem>
#include <string>
#include <vector>

namespace SparkEditor
{

    class GameModuleSelectorPanel;

    /// @brief Editor panel for launching, monitoring, and stopping real out-of-process
    /// SparkEngine.exe play-test instances.
    class PlayControlPanel : public EditorPanel
    {
      public:
        PlayControlPanel();
        ~PlayControlPanel() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "PlayControlPanel"; }

        /// @brief Wire the Game Module Selector panel so the play bar launches
        /// whichever module the user has selected there, instead of duplicating
        /// module discovery/probing. Set once during panel creation; nullptr is
        /// tolerated (buttons stay disabled with an explanatory status line).
        void SetGameModuleSelector(GameModuleSelectorPanel* selector) { m_gameModuleSelector = selector; }

      private:
        enum class InstanceRole
        {
            Game,
            Dedicated,
            Client
        };

        struct RunningInstance
        {
            void* processHandle = nullptr; ///< HANDLE, owned — CloseHandle on removal.
            unsigned long pid = 0;
            InstanceRole role = InstanceRole::Game;
            std::string label; ///< Module name (+ extra context, e.g. bot count / target).
            bool alive = true;
            unsigned long exitCode = 0;
        };

        // -- Rendering --
        void RenderPlayBar();
        void RenderStatus();
        void RenderQuickConnect();
        static const char* RoleLabel(InstanceRole role);

        // -- Launch actions --
        void LaunchGame();
        void LaunchDedicatedWithBots();
        void LaunchClient(const std::string& hostPort);
        void StopAll();

        // -- Bookkeeping --
        void PollInstances();
        void RefreshLogTail();
        void SetLaunchContextDirectory(const std::filesystem::path& workingDirectory);
        std::string WriteDedicatedBotsCfg(int botCount);
        std::string WriteConnectCfg(const std::string& hostPort);

        GameModuleSelectorPanel* m_gameModuleSelector = nullptr;

        std::vector<RunningInstance> m_instances;
        int m_launchCounter = 0; ///< Used to make temp cfg filenames unique across launches.

        int m_botCount = 8;
        char m_quickConnectBuf[128] = "127.0.0.1:27020";

        // -- exec_audit.log tail --
        std::filesystem::path m_logPath;
        std::vector<std::string> m_logLines;
        long long m_logReadOffset = 0;
        float m_logPollTimer = 0.0f;
        static constexpr size_t kMaxLogLines = 500;

        std::string m_statusMessage;
    };

} // namespace SparkEditor
