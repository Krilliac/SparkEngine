/**
 * @file ModSystem.h
 * @brief Plugin and modding SDK for user-created content
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides a mod loading and management system that allows user-created
 * content to be loaded at runtime. Mods can include scripts, assets,
 * entity definitions, and configuration overrides.
 *
 * ## Mod structure
 * ```
 * MyMod/
 *   ├── mod.json          (metadata: name, version, author, dependencies)
 *   ├── Scripts/          (AngelScript files)
 *   ├── Assets/           (meshes, textures, sounds)
 *   ├── Data/             (JSON config overrides)
 *   └── Preview.png       (mod preview image)
 * ```
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace Spark
{

    // =============================================================================
    // ModInfo
    // =============================================================================

    /**
 * @brief Metadata for a single mod.
 */
    struct ModInfo
    {
        std::string id;                        ///< Unique mod identifier
        std::string name;                      ///< Display name
        std::string author;                    ///< Mod author
        std::string version;                   ///< Semantic version string
        std::string description;               ///< Mod description
        std::string path;                      ///< Filesystem path to mod directory
        std::string previewImage;              ///< Path to preview image
        std::vector<std::string> dependencies; ///< IDs of required mods
        bool enabled = false;                  ///< Whether this mod is enabled
        bool loaded = false;                   ///< Whether this mod is currently loaded
        int loadOrder = 0;                     ///< Load priority (lower = earlier)
    };

    // =============================================================================
    // ModState
    // =============================================================================

    enum class ModState
    {
        Available, ///< Found on disk, not loaded
        Loading,   ///< Currently being loaded
        Active,    ///< Loaded and active
        Error,     ///< Failed to load
        Disabled   ///< Explicitly disabled by user
    };

    // =============================================================================
    // ModSystem
    // =============================================================================

    /**
 * @class ModSystem
 * @brief Discovers, loads, and manages game modifications.
 */
    class ModSystem
    {
      public:
        ModSystem();
        ~ModSystem() = default;

        /**
     * @brief Scan a directory for available mods.
     * @param modsDirectory Path to scan (e.g. "Data/Mods/").
     * @return Number of mods discovered.
     */
        size_t ScanForMods(const std::string& modsDirectory);

        /**
     * @brief Enable a mod by ID.
     * @param modId Mod identifier.
     * @return true if the mod was found and enabled.
     */
        bool EnableMod(const std::string& modId);

        /**
     * @brief Disable a mod by ID.
     * @param modId Mod identifier.
     */
        void DisableMod(const std::string& modId);

        /**
     * @brief Load all enabled mods in dependency order.
     * @return true if all mods loaded successfully.
     */
        bool LoadEnabledMods();

        /**
     * @brief Unload all mods.
     */
        void UnloadAll();

        /**
     * @brief Load a single mod.
     * @param modId Mod identifier.
     * @return true if loading succeeded.
     */
        bool LoadMod(const std::string& modId);

        /**
     * @brief Unload a single mod.
     * @param modId Mod identifier.
     */
        void UnloadMod(const std::string& modId);

        // --- Query ---

        /** @brief Get all discovered mods. */
        std::vector<ModInfo> GetAllMods() const;

        /** @brief Get enabled mods. */
        std::vector<ModInfo> GetEnabledMods() const;

        /** @brief Get mod info by ID. */
        const ModInfo* GetModInfo(const std::string& modId) const;

        /** @brief Get mod state. */
        ModState GetModState(const std::string& modId) const;

        /** @brief Check if a mod is loaded and active. */
        bool IsModActive(const std::string& modId) const;

        // --- Load order ---

        /**
     * @brief Set the load order for enabled mods.
     * @param orderedModIds Mod IDs in desired load order.
     */
        void SetLoadOrder(const std::vector<std::string>& orderedModIds);

        /**
     * @brief Check if all dependencies for a mod are satisfied.
     * @param modId Mod to check.
     * @return true if all dependencies are enabled and available.
     */
        bool AreDependenciesMet(const std::string& modId) const;

        // --- Persistence ---

        /**
     * @brief Save mod configuration (enabled/disabled, load order) to file.
     * @param filePath Configuration file path.
     */
        bool SaveConfig(const std::string& filePath) const;

        /**
     * @brief Load mod configuration from file.
     * @param filePath Configuration file path.
     */
        bool LoadConfig(const std::string& filePath);

        // --- Callbacks ---

        /** @brief Register callback for mod load events. */
        void OnModLoaded(std::function<void(const std::string&)> callback);

        /** @brief Register callback for mod unload events. */
        void OnModUnloaded(std::function<void(const std::string&)> callback);

        // --- Console integration ---

        /** @brief Get mod system status (console integration). */
        std::string Console_GetStatus() const;

        /** @brief List all mods (console integration). */
        std::string Console_ListMods() const;

      private:
        bool ParseModJson(const std::string& path, ModInfo& info);

        std::unordered_map<std::string, ModInfo> m_mods;
        std::unordered_map<std::string, ModState> m_modStates;
        std::vector<std::function<void(const std::string&)>> m_loadCallbacks;
        std::vector<std::function<void(const std::string&)>> m_unloadCallbacks;
        std::string m_modsDirectory;
    };

} // namespace Spark
