/**
 * @file GameModuleSelectorPanel.h
 * @brief Panel for discovering and selecting which game modules to load
 * @author Spark Engine Team
 * @date 2026
 *
 * When multiple game modules are present (e.g., SparkGame + SparkGameMMO),
 * this panel lets the user see which modules are available on disk, which
 * are currently loaded, and toggle modules on/off. It can also generate
 * or update the spark.modules.json manifest for persistent selection.
 *
 * Accessible via Window > Game Module Selector.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

// Forward declare — avoids pulling ModuleManager.h into the header
struct DiscoveredModule;

namespace SparkEditor
{

    /**
     * @brief Editor panel for discovering and managing game module DLLs
     *
     * Features:
     * - Scans the engine's output directory for available module DLLs
     * - Shows loaded vs. available modules with version info
     * - Allows toggling which modules are active
     * - Generates spark.modules.json for persistent module selection
     * - Shows module metadata (name, version, load order)
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

      private:
        void RefreshModuleList();
        void RenderModuleList();
        void RenderManifestControls();
        void SaveModuleManifest();

        struct ModuleEntry
        {
            std::string name;
            std::string path;
            std::string version;
            bool isLoaded = false;
            bool isSelected = false; ///< User selection for manifest generation
        };

        std::vector<ModuleEntry> m_modules;
        float m_refreshTimer = 0.0f;
        bool m_needsRefresh = true;
        std::string m_statusMessage;
    };

} // namespace SparkEditor
