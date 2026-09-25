/**
 * @file VisualScriptDemoWorld.h
 * @brief Fail-fast builder for the visual-script demo world (script validation, spawn, rollback).
 *
 * The module shell (Main.cpp) owns one DemoWorld per load. It is kept separate
 * from the IModule so the load-rejection and rollback contract can be driven
 * directly by SparkTests against a real World and AngelScriptEngine.
 */

#pragma once

#include "Engine/ECS/Components/CoreComponents.h"

#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

class World;
class AngelScriptEngine;

namespace Spark::VisualScriptDemo
{
    /**
     * @brief Loads the five-script manifest and builds (or rolls back) the demo's 11 script entities.
     *
     * Every failure is fail-fast: it leaves no VS_ entity, no attached script
     * instance and no world bound to the script API by this builder, and it
     * records an actionable diagnostic that names the script file (and the
     * source line when the fault has one) in GetLastError().
     */
    class DemoWorld
    {
      public:
        /**
         * @param world        Live ECS world the demo entities are created in (must outlive this object).
         * @param scriptEngine Initialized AngelScript engine (must outlive this object).
         */
        DemoWorld(World& world, AngelScriptEngine& scriptEngine);

        /// Destroys any entities still owned by this builder.
        ~DemoWorld();

        DemoWorld(const DemoWorld&) = delete;
        DemoWorld& operator=(const DemoWorld&) = delete;

        /**
         * @brief Select the first search root holding the complete manifest, then compile and validate every script.
         * @param searchPaths Candidate script directories, in priority order.
         * @return false (with GetLastError() set) on a missing file, compile error or bad selfEntity placeholder.
         */
        bool LoadScripts(std::span<const std::filesystem::path> searchPaths);

        /**
         * @brief Spawn all demo entities, bind each validated script to its entity and call Start().
         *
         * Entities from a previous Spawn() are destroyed first, so this also
         * restarts the demo. Binds the world to the AngelScript API. A partial demo is a failure:
         * every entity created by this call is destroyed and the world binding
         * made here is released before returning false.
         */
        bool Spawn();

        /// Detach and destroy every entity this builder created (reverse creation order).
        void DestroyEntities();

        /// Entities currently owned by the builder, in creation order.
        const std::vector<EntityID>& GetEntities() const { return m_entities; }

        /// Script directory chosen by the last successful LoadScripts().
        const std::filesystem::path& GetScriptRoot() const { return m_scriptRoot; }

        /// Diagnostic for the most recent failure (empty after a success).
        const std::string& GetLastError() const { return m_lastError; }

      private:
        bool AttachScript(EntityID entity, const std::string& className);
        void Fail(const std::string& message);

        World& m_world;
        AngelScriptEngine& m_scriptEngine;
        std::filesystem::path m_scriptRoot;
        std::unordered_map<std::string, std::string> m_scriptSources;
        std::vector<EntityID> m_entities;
        std::string m_lastError;
    };
} // namespace Spark::VisualScriptDemo
