/**
 * @file ModdingPanel.h
 * @brief Mod management and hot-reload panel
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for discovering, enabling, and managing game mods
     *
     * Shows available mods with enable/disable toggles, load order management,
     * dependency checking, and mod preview information.
     */
    class ModdingPanel : public EditorPanel
    {
      public:
        ModdingPanel();
        ~ModdingPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct ModEntry
        {
            char id[64] = {};
            char name[128] = {};
            char author[64] = {};
            char version[32] = {};
            char description[256] = {};
            bool enabled = false;
            bool loaded = false;
            int loadOrder = 0;
            bool dependenciesMet = true;
        };

        void RenderModList();
        void RenderModDetails();
        void RenderLoadOrder();

        std::vector<ModEntry> m_mods;
        int m_selectedMod = -1;
        char m_modsDirectory[256] = "Mods/";
    };

} // namespace SparkEditor
