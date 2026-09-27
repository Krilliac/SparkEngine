/**
 * @file PrefabManager.h
 * @brief Manages prefab assets — creation, instantiation, saving, and loading
 * @author Spark Engine Team
 * @date 2025
 *
 * The PrefabManager provides CRUD operations on prefab assets and handles
 * tracking of prefab instances and their overrides.
 */

#pragma once

#include "PrefabAsset.h"
#include "../SceneSystem/SceneFile.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <string>
#include <vector>
#include <functional>

namespace SparkEditor
{

    /**
     * @brief Tracks a prefab instance in the scene
     */
    struct PrefabInstance
    {
        uint64_t entityId = 0;                 ///< The scene entity this instance represents
        std::string sourcePrefabName;          ///< Name of the source prefab
        std::vector<PrefabOverride> overrides; ///< Properties that differ from the source
    };

    /**
     * @brief Manages prefab assets and their instances
     *
     * Provides creation from entities, instantiation into scenes,
     * save/load from disk, and override tracking.
     */
    class PrefabManager
    {
      public:
        PrefabManager() = default;
        ~PrefabManager() = default;

        // Non-copyable
        PrefabManager(const PrefabManager&) = delete;
        PrefabManager& operator=(const PrefabManager&) = delete;

        /**
         * @brief Initialize the prefab manager
         * @return true on success
         */
        bool Initialize();

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Create a prefab from an existing entity
         * @param entityId The entity to snapshot as a prefab
         * @param prefabName Name for the new prefab
         * @return Pointer to the created prefab, or nullptr on failure
         */
        PrefabAsset* CreatePrefabFromEntity(uint64_t entityId, const std::string& prefabName);

        /**
         * @brief Create a new empty prefab
         * @param name Name for the new prefab
         * @return Pointer to the created prefab
         */
        PrefabAsset* CreateEmptyPrefab(const std::string& name);

        /**
         * @brief Instantiate a prefab into the scene
         * @param prefabName Name of the prefab to instantiate
         * @return Entity ID of the new instance, or 0 on failure
         */
        uint64_t InstantiatePrefab(const std::string& prefabName);

        /**
         * @brief Save a prefab to `<directory>/<name>.sparkprefab`
         * @param name Prefab name; must be a single safe file-name segment (no separators, ':', or "..")
         * @param directory UTF-8 directory to save into. Empty means the project prefab directory,
         *                  which is created on demand; with no project open the save fails rather
         *                  than writing into the process working directory.
         * @return true on success
         */
        bool SavePrefab(const std::string& name, const std::string& directory = "");

        /**
         * @brief Set the open project's prefab directory (`<project>/Prefabs`); empty when no project is open
         */
        void SetProjectPrefabDirectory(std::filesystem::path directory);

        /**
         * @brief Load every `*.sparkprefab` directly in the project prefab directory, in file-name order
         *
         * Each file goes through LoadPrefab, so a damaged file falls back to its `.bak` and a
         * rejected file leaves any already-loaded prefab of the same name in place.
         *
         * The directory comes from an untrusted project, so the sweep is bounded: symbolic links
         * are rejected unread by TryLoad, at most kMaxProjectPrefabFiles files are considered, and
         * a file whose readable bytes (primary plus `.bak`) would take the sweep past
         * kMaxProjectPrefabBytes is skipped. A file over PrefabAsset::kMaxPrefabFileBytes is
         * rejected unread and costs nothing. Anything left out is reported in @p diagnostics.
         *
         * @param diagnostics Receives one actionable message per rejected file and per file loaded
         *                    from its `.bak`, plus one per limit that left files unloaded
         * @return Number of prefabs loaded
         */
        size_t LoadProjectPrefabs(std::vector<std::string>& diagnostics);

        /// Most `*.sparkprefab` files LoadProjectPrefabs considers in one project.
        static constexpr size_t kMaxProjectPrefabFiles = 2048;
        /// Most bytes of project prefab files LoadProjectPrefabs reads in one sweep.
        static constexpr std::uintmax_t kMaxProjectPrefabBytes = 32u * 1024u * 1024u;

        /**
         * @brief Load a prefab from disk (see PrefabAsset::TryLoad for validation and recovery)
         * @param filePath UTF-8 path to the .sparkprefab file
         * @param error When non-null, receives the actionable reason on failure, or why the
         *              primary was rejected when the retained `.bak` was loaded instead
         * @return Pointer to the loaded prefab, or nullptr on failure (the registry is unchanged)
         */
        PrefabAsset* LoadPrefab(const std::string& filePath, std::string* error = nullptr);

        /**
         * @brief Delete a prefab by name
         * @param name Prefab name to delete
         * @return true if found and deleted
         */
        bool DeletePrefab(const std::string& name);

        /**
         * @brief Get a prefab by name
         * @param name Prefab name
         * @return Pointer to the prefab, or nullptr if not found
         */
        PrefabAsset* GetPrefab(const std::string& name);

        /**
         * @brief Get a prefab by name (const)
         * @param name Prefab name
         * @return Const pointer to the prefab, or nullptr if not found
         */
        const PrefabAsset* GetPrefab(const std::string& name) const;

        /**
         * @brief Get all loaded prefabs
         * @return Const reference to the prefab map
         */
        const std::unordered_map<std::string, PrefabAsset>& GetPrefabs() const { return m_prefabs; }

        /**
         * @brief Get all prefab names
         * @return Vector of prefab names
         */
        std::vector<std::string> GetPrefabNames() const;

        /**
         * @brief Get the number of loaded prefabs
         * @return Prefab count
         */
        size_t GetPrefabCount() const { return m_prefabs.size(); }

        /**
         * @brief Track a new prefab instance
         * @param entityId Entity ID of the instance
         * @param prefabName Source prefab name
         */
        void RegisterInstance(uint64_t entityId, const std::string& prefabName);

        /**
         * @brief Remove instance tracking for an entity
         * @param entityId Entity ID to unregister
         */
        void UnregisterInstance(uint64_t entityId);

        /**
         * @brief Get all instances of a specific prefab
         * @param prefabName Prefab name
         * @return Vector of instance records
         */
        std::vector<PrefabInstance> GetInstances(const std::string& prefabName) const;

        /**
         * @brief Apply the prefab template's transform to all its instances
         *
         * Only properties an instance has NOT overridden are re-applied. Instances whose scene object is
         * gone, and every instance when no scene is set, are not counted as updated.
         *
         * SceneObject stores only a Transform plus a list of component types, so this syncs the transform;
         * per-component property sync needs per-component storage in SceneObject (see PrefabManager docs).
         *
         * @param prefabName Prefab name whose instances should be updated
         * @return Number of instances actually written to
         */
        int ApplyPrefabToInstances(const std::string& prefabName);

        /**
         * @brief Record that an instance property differs from its template
         *
         * ApplyPrefabToInstances skips every property recorded here, which is what makes a per-instance edit
         * survive a template change.
         *
         * @param entityId Entity ID of the instance
         * @param componentType Component the property belongs to (e.g. "Transform")
         * @param propertyName Property that now differs (e.g. "position")
         * @param value The instance's value
         * @return true if the instance was found
         */
        bool SetInstanceOverride(uint64_t entityId, const std::string& componentType, const std::string& propertyName,
                                 const PrefabPropertyValue& value);

        /**
         * @brief Drop an instance's overrides and restore it to the template
         *
         * Returns true once the instance was found and its overrides were cleared — that state change
         * is irreversible, so it must not be reported as a failure. A template transform that could not
         * be written back (no Transform component, nothing stored, missing SceneObject) is logged.
         *
         * @param entityId Entity ID of the instance
         * @return true if the instance was found and its overrides were cleared
         */
        bool RevertInstance(uint64_t entityId);

        /**
         * @brief Drop a single overridden property on an instance and restore that property
         * @param entityId Entity ID of the instance
         * @param componentType Component the property belongs to (e.g. "Transform")
         * @param propertyName Property to revert (e.g. "position")
         * @return true if a matching override was found and removed
         */
        bool RevertProperty(uint64_t entityId, const std::string& componentType, const std::string& propertyName);

        /**
         * @brief Set callback for when prefab list changes
         * @param callback Notification callback
         */
        void SetOnPrefabsChanged(std::function<void()> callback) { m_onPrefabsChanged = std::move(callback); }

        /// @brief Set the active scene for entity queries and instantiation.
        void SetScene(SceneFile* scene) { m_scene = scene; }

      private:
        std::unordered_map<std::string, PrefabAsset> m_prefabs;
        std::vector<PrefabInstance> m_instances;
        std::function<void()> m_onPrefabsChanged;
        SceneFile* m_scene = nullptr;                   ///< Non-owning pointer to the active scene
        std::filesystem::path m_projectPrefabDirectory; ///< Empty while no project is open

        void NotifyPrefabsChanged();

        /**
         * @brief Write a prefab template's transform onto one scene object
         * @param prefab Source template
         * @param instance Instance record (its overrides are respected)
         * @return true if the instance's scene object was found and written
         */
        bool ApplyTemplateTransform(const PrefabAsset& prefab, const PrefabInstance& instance);

        /**
         * @brief Create sample prefabs for demonstration
         */
        void CreateSamplePrefabs();
    };

} // namespace SparkEditor
