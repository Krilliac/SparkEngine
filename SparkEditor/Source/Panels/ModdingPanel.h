/**
 * @file ModdingPanel.h
 * @brief Mod management and hot-reload panel
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Engine/Modding/ModSystem.h"
#include <memory>
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for discovering, enabling, and managing game mods
     *
     * Every action runs against a real Spark::ModSystem: the one registered in the
     * EngineContext when a game session is live, otherwise a panel-owned instance
     * used for editor-side browsing of the project's mods directory.
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

        /// @brief Scan @p directory through the ModSystem; returns the number of mods found.
        size_t ScanForMods(const std::string& directory);

        /// @brief Enable or disable a discovered mod; returns false if the id is unknown.
        bool SetModEnabled(const std::string& modId, bool enabled);

        /// @brief Unload everything and reload the enabled mods; returns the ModSystem result.
        bool ReloadAll();

        /// @brief Mods currently mirrored from the ModSystem, ordered by load order.
        const std::vector<Spark::ModInfo>& GetMods() const { return m_mods; }

      private:
        /// @brief The ModSystem this panel drives (EngineContext's when a session is live).
        Spark::ModSystem& System();

        /// @brief Re-read the mod list from the ModSystem into m_mods.
        void RefreshFromSystem();

        void RenderModList();
        void RenderModDetails();
        void RenderLoadOrder();

        /// Editor-side ModSystem, used when no engine session has registered one.
        std::unique_ptr<Spark::ModSystem> m_ownedModSystem;

        std::vector<Spark::ModInfo> m_mods;
        int m_selectedMod = -1;
        char m_modsDirectory[256] = "Mods/";
    };

} // namespace SparkEditor
