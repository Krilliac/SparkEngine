/**
 * @file ScriptEditorPanel.h
 * @brief AngelScript editor and hot-reload management panel
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for managing AngelScript files, hot-reload, and script debugging
     *
     * Shows compiled modules, hot-reload status, script instances per entity,
     * and hook listeners. Supports triggering manual recompilation.
     */
    class ScriptEditorPanel : public EditorPanel
    {
      public:
        ScriptEditorPanel();
        ~ScriptEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct ScriptModuleInfo
        {
            char name[128] = {};
            char filePath[256] = {};
            int entityCount = 0;
            bool compiled = false;
        };

        struct HookListenerInfo
        {
            char hookType[64] = {};
            char scriptName[128] = {};
            int priority = 0;
        };

        void RenderModuleList();
        void RenderHotReloadStatus();
        void RenderHookListeners();
        void RenderScriptConsole();

        std::vector<ScriptModuleInfo> m_modules;
        std::vector<HookListenerInfo> m_hooks;
        std::vector<std::string> m_compileLog;
        int m_selectedModule = -1;
        bool m_hotReloadEnabled = true;
        int m_watchedFiles = 0;
        int m_recompileCount = 0;
        int m_errorCount = 0;
        char m_scriptFilter[128] = {};
    };

} // namespace SparkEditor
