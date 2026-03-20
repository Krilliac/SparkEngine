/**
 * @file SceneConfigDatabase.h
 * @brief Per-scene configuration override database
 *
 * Stores per-scene/per-level overrides
 * for render settings, physics parameters, audio configuration, and
 * known workarounds. Overrides are applied at scene load time.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Render setting overrides for a specific scene.
     */
    struct SceneRenderOverrides
    {
        std::optional<float> shadowDistance;
        std::optional<int> shadowCascades;
        std::optional<float> drawDistance;
        std::optional<float> lodBias;
        std::optional<int> maxPointLights;
        std::optional<bool> enableVolumetrics;
        std::optional<bool> enableSSAO;
        std::optional<bool> enableSSR;
    };

    /**
     * @brief Physics setting overrides for a specific scene.
     */
    struct ScenePhysicsOverrides
    {
        std::optional<float> gravity;
        std::optional<int> subSteps;
        std::optional<float> fixedTimestep;
        std::optional<float> contactOffset;
    };

    /**
     * @brief Audio setting overrides for a specific scene.
     */
    struct SceneAudioOverrides
    {
        std::optional<std::string> reverbPreset;
        std::optional<float> masterVolume;
        std::optional<float> ambientVolume;
        std::optional<float> musicVolume;
    };

    /**
     * @brief Complete configuration entry for a single scene.
     */
    struct SceneConfigEntry
    {
        std::string sceneId;
        std::string displayName;
        std::string description;
        SceneRenderOverrides render;
        ScenePhysicsOverrides physics;
        SceneAudioOverrides audio;
        std::unordered_map<std::string, std::string> customOverrides;
    };

    /**
     * @brief Database mapping scene IDs to configuration overrides.
     *
     * Load overrides when a scene loads to apply level-specific settings.
     * Useful for shipped games where different levels have different
     * performance characteristics.
     */
    class SceneConfigDatabase
    {
      public:
        static SceneConfigDatabase& GetInstance()
        {
            static SceneConfigDatabase instance;
            return instance;
        }

        void Initialize() { m_initialized = true; }
        void Shutdown()
        {
            m_entries.clear();
            m_initialized = false;
        }

        /**
         * @brief Register a scene configuration entry.
         */
        void RegisterScene(const SceneConfigEntry& entry) { m_entries[entry.sceneId] = entry; }

        /**
         * @brief Remove a scene configuration entry.
         */
        void UnregisterScene(const std::string& sceneId) { m_entries.erase(sceneId); }

        /**
         * @brief Look up overrides for a scene. Returns nullptr if not found.
         */
        const SceneConfigEntry* GetConfig(const std::string& sceneId) const
        {
            auto it = m_entries.find(sceneId);
            return it != m_entries.end() ? &it->second : nullptr;
        }

        bool HasConfig(const std::string& sceneId) const { return m_entries.count(sceneId) > 0; }

        /**
         * @brief Get a custom override value for a scene.
         */
        std::optional<std::string> GetCustomOverride(const std::string& sceneId, const std::string& key) const
        {
            auto it = m_entries.find(sceneId);
            if (it == m_entries.end())
                return std::nullopt;
            auto ov = it->second.customOverrides.find(key);
            if (ov == it->second.customOverrides.end())
                return std::nullopt;
            return ov->second;
        }

        size_t GetEntryCount() const { return m_entries.size(); }
        bool IsInitialized() const { return m_initialized; }

        std::vector<std::string> GetAllSceneIds() const
        {
            std::vector<std::string> ids;
            ids.reserve(m_entries.size());
            for (const auto& [id, entry] : m_entries)
                ids.push_back(id);
            return ids;
        }

        std::string Console_GetReport() const
        {
            std::string report = "Scene Config Database: " + std::to_string(m_entries.size()) + " entries\n";
            for (const auto& [id, entry] : m_entries)
            {
                report += "  " + id;
                if (!entry.displayName.empty())
                    report += " (" + entry.displayName + ")";
                report += "\n";
            }
            return report;
        }

      private:
        SceneConfigDatabase() = default;

        std::unordered_map<std::string, SceneConfigEntry> m_entries;
        bool m_initialized = false;
    };

} // namespace Spark
